/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTBuiltinIDTypes.h"
#include "AOTTypeIDs.h"
#include "AOTVector.h"
#include <algorithm>
#include <array>
#include <optional>
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>

namespace JSC { namespace AOT { namespace Typed {
struct ObjectRecord;
} } }

namespace WTF {

// Whoever grows the table of objects writes every new record itself, the sites on many threads side by side: nothing is laid down for them first.
template<> struct VectorTraits<JSC::AOT::Typed::ObjectRecord> : VectorTraitsBase<false, void> {
    static constexpr bool needsInitialization = false;
    static constexpr bool canMoveWithMemcpy = true;
};

} // namespace WTF

namespace JSC { namespace AOT {

class BuiltinTable;
class ParallelLoops;

namespace Typed {

class Program;

// What stands for the objects of a run. Two ids may stand for one object: mayBeSameObject() in AOTTypeRelations.h.
enum class ObjectKind : uint8_t {
    // One: the owner of the cells of `unknown`. It is a member of no set. That a type holds `unknown` is TypeParts' anyObject, anyFunction, unnamedLayouts and unnamedClasses.
    Unknown,
    // By the table of built-ins.
    Builtin,
    ResultOfBuiltin,
    HostObject,
    EscapedFunctionsOfHolder,
    EscapedOfClass,
    // Two: one for each MakerOfShapelessObject.
    EscapedWithoutShape,
    // Five of each function.
    Closure,
    CoroutineResult,
    RestArray,
    FunctionPrototype,
    ConstructedObject,
    // By the program's other lists.
    Namespace,
    ModuleBuiltin,
    Instance,
    Prototype,
    EscapedOfLayout,
    // By the scan of bodies.
    Site,
};

enum class ObjectTrait : uint16_t {
    // An object has exactly one of these three. One that has a layout has no class here, even where its class extends a built-in class: Layout::writtenBuiltinBase says that.
    HasLayout = 1 << 0,
    HasClass = 1 << 1,
    // Neither a layout nor a class that a test could ask for: a closure, a prototype object, a dictionary, most built-ins.
    HasNoShape = 1 << 2,
    // And exactly one of these three.
    IsClosure = 1 << 3,
    IsBuiltin = 1 << 4,
    IsOtherObject = 1 << 5,
    IsCallable = 1 << 6,
    // It stands for every escaped object of a layout, a class, a holder or a maker of objects without a shape. Beside `unknown` it is a prediction, never a proof.
    IsMerged = 1 << 7,
    // One cell in a run of one global, which a comparison of pointers finds.
    IsFoundByIdentity = 1 << 8,
    // `new.target` may give it a layout below the one named here.
    MayHaveLayoutBelow = 1 << 9,
    // Made for one instance of a small summary. sharedForm names the object of the function's shared nodes.
    IsOfInstance = 1 << 10,
    // Every row that makes it makes an ordinary object: the records of JSON.parse(), a descriptor. No array, no function, no proxy, no cell of another class.
    IsPlainObjectOfRows = 1 << 11,
};

inline constexpr OptionSet<ObjectTrait> traitsOfShape { ObjectTrait::HasLayout, ObjectTrait::HasClass, ObjectTrait::HasNoShape };
inline constexpr OptionSet<ObjectTrait> traitsOfIdentity { ObjectTrait::IsClosure, ObjectTrait::IsBuiltin, ObjectTrait::IsOtherObject };

enum class SiteOrigin : uint8_t { Operation, DataNode, BuiltinResult };

// Who made an object that has neither a layout nor a class and whose record does not call it a function.
// Of what the host makes nobody says what it is. What an operation, the data of the program or a row of a built-in makes is no function.
enum class MakerOfShapelessObject : uint8_t { Host, OperationOrRow };

enum class SiteTrait : uint8_t {
    NamesPrototype = 1 << 0,
    IsProxyFromNamedConstructor = 1 << 1,
    IsDenseArrayLiteral = 1 << 2,
    IsPairLiteral = 1 << 3,
    IsAnswerOfCallWithOneArgumentAtMost = 1 << 4,
    // The array that NewTemplateObject makes, and the array under its `raw`. Each is one object for its place in the source: no instance of a summary has a copy of it.
    IsTemplateObject = 1 << 5,
};

struct ObjectRecord {
    ObjectKind kind { ObjectKind::Site };
    SiteOrigin origin { SiteOrigin::Operation };
    OptionSet<ObjectTrait> traits;
    // Builtin, ResultOfBuiltin, EscapedFunctionsOfHolder, ModuleBuiltin: the canonical BuiltinID. HostObject: the index of its record. EscapedOfClass: the BuiltinClassID. EscapedWithoutShape: the MakerOfShapelessObject.
    // The five of a function: the FunctionID. Namespace: the ModuleID. Instance, Prototype, EscapedOfLayout: the LayoutID. Site: the ValueID in `owner`.
    uint32_t payload { 0 };
    LayoutID layout;
    BuiltinClassID builtinClass;
    // ResultOfBuiltin: which of the four. Site: which of the objects that one operation makes, or the node of its data in preorder, where every node counts, leaves and holes too.
    uint32_t ordinal { 0 };
    // Site: the function whose body holds it. ModuleBuiltin: unused.
    FunctionID owner;
    // ModuleBuiltin: the module.
    ModuleID module;
    ObjectID sharedForm;
    // IsOfInstance: ObjectOfInstance::instance.
    uint32_t instance { 0 };
    OptionSet<SiteTrait> siteTraits;

    bool has(ObjectTrait trait) const { return traits.contains(trait); }
    bool has(SiteTrait trait) const { return siteTraits.contains(trait); }
};

// Behind what a row answers and what lies inside that: the functions that a descriptor of a native accessor holds under `get` and under `set`.
// One object stands for every function that the engine makes for that half of the accessor. A call of it runs the row of the accessor on the receiver of the call.
inline constexpr uint32_t ordinalOfGetterFunctionOfAccessor = 2;
inline constexpr uint32_t ordinalOfSetterFunctionOfAccessor = 3;

inline bool isFunctionOfNativeAccessor(const ObjectRecord& record)
{
    return record.kind == ObjectKind::ResultOfBuiltin && record.ordinal >= ordinalOfGetterFunctionOfAccessor;
}

// What the scan says of one allocation in a body. Sorted by (value, ordinal) within a function.
struct ObjectSite {
    ValueID value;
    uint32_t ordinal { 0 };
    SiteOrigin origin { SiteOrigin::Operation };
    OptionSet<SiteTrait> traits;
    LayoutID layout;
    BuiltinClassID builtinClass;
    bool mayHaveLayoutBelow { false };
    // Of SiteOrigin::DataNode: the record whose node it is.
    DataID data;
};

// `const config: Config = JSON.parse(text)`: what a parser by a written type makes at a call, each a site of SiteOrigin::BuiltinResult that stands for every such object that the call answers with.
// From this ordinal on: one for each layout that the parser makes, ascending by layout, one for each type with an array, in the order of FactTables::typedResultAt(), and the array of untyped values.
inline constexpr uint32_t firstOrdinalOfTypedResult = 2;
// ``await sql<User[]>`select * from users` ``: the rows that the host makes for the query that a call answers with, each a site of SiteOrigin::BuiltinResult that has escaped from birth.
// From this ordinal on, by the place of the layout in ExpectedResult::layouts. No typed result has as many objects, so the two never meet at one call.
inline constexpr uint32_t firstOrdinalOfRowMadeByHost = 1u << 31;

// A fresh object: what `sharedForm` stands for where its creator is entered through one call as written. Statements absorbed there and the walk's inline frames name the same object.
// It is not always all that is made through the call. A copy of the caller in which the creator is one of several callees enters its shared nodes there: the object is `sharedForm`.
// So an inline frame names the copy only where Facts::copyOf() answers it.
struct ObjectOfInstance {
    // The number of that call: an index into FactGraph::callsEnteredThrough(). The solver gives it out in an order that depends on the program alone.
    uint32_t instance { 0 };
    ObjectID sharedForm;

    friend constexpr auto operator<=>(const ObjectOfInstance&, const ObjectOfInstance&) = default;
};

// It evaluates text, loads code or lets in unseen code. `unknown` never stands for such a function, escaped or not.
bool callOrReadOfBuiltinIsEvent(const BuiltinTable&, BuiltinID canonical);
// A rule of Facts does something for this very function that it does for no other callee: a call of it opens a typed result. So it stays apart when it escapes, and it stays listed beside `unknown`.
bool rulesMeetBuiltinByName(const BuiltinTable&, BuiltinID canonical);

// What the wire says of a function before its record is read. A large set pays for each chunk of numbers that it touches, so the objects that stand in large sets lie together:
// each of the five kinds is numbered group by group, and by function within a group. A signature has no body and none of the five is ever made: its numbers come last.
struct FunctionInOrderOfObjects {
    // Of the closure, the prototype object and the constructed object. Ascending.
    enum class Group : uint8_t { PlainFunctionOrClass, Arrow, MethodOrAccessor, EnteredByEngineAlone, WithoutBody };
    static constexpr unsigned numberOfGroups = static_cast<unsigned>(Group::WithoutBody) + 1;

    Group group { Group::PlainFunctionOrClass };
    bool isCoroutine { false };
    bool hasRestArray { false };
};

// Every abstract object of the build, numbered kind by kind in the order of ObjectKind, then the objects of instances in the order they were added.
// The numbers depend on the program alone.
class ObjectTable {
    WTF_MAKE_NONCOPYABLE(ObjectTable);
public:
    // Before a type is read: the objects that a wire type can name are those of built-ins and functions.
    // The five records of each function are written on every thread of the loops. Null: on the calling thread.
    JS_EXPORT_PRIVATE ObjectTable(const BuiltinTable&, std::span<const FunctionInOrderOfObjects>, const ParallelLoops* = nullptr);

    // Once, when every list of the program is read and no layout will be added.
    // `require`, `import.meta` and what hangs below them get objects of a module only where a ModuleBuiltin in some body names that module: nothing else brings the first of them into sight.
    JS_EXPORT_PRIVATE void addObjectsOfProgram(const Program&, const BitVector& modulesNamedByModuleBuiltins, const ParallelLoops&);
    // Once, behind the scan, with a number for each function of the program. Then each function's sites are written by whoever scanned it, in parallel.
    JS_EXPORT_PRIVATE void reserveSites(std::span<const uint32_t> numberOfSitesOfFunctions, const ParallelLoops&);
    JS_EXPORT_PRIVATE void defineSites(FunctionID, std::span<const ObjectSite> sortedByValueAndOrdinal);
    // By Program::appendParsersFor(), on one thread, in front of reserveSites() or behind it, for the functions that the program has got since the table was made: none of them has a site.
    // sitesOf() is all that may be asked of one. The five objects of a function are those of the functions that the constructor has counted.
    void addFunctionsWithoutSites(uint32_t numberOfFunctions)
    {
        RELEASE_ASSERT(!m_firstSiteOfFunction.isEmpty());
        while (m_firstSiteOfFunction.size() <= numberOfFunctions)
            m_firstSiteOfFunction.append(uint32_t { m_firstSiteOfFunction.last() });
    }
    // Between two rounds of the solve, by one thread. Answers the first of the new numbers, which follow the order of the list: the order in which the solver's logs first asked for them.
    // Each gets a copy of the record of its shared form, with IsOfInstance, sharedForm and instance.
    JS_EXPORT_PRIVATE ObjectID addObjectsOfInstances(std::span<const ObjectOfInstance> withoutRepeats);
    // The same in two steps. By one thread, while no other looks at the table: room for that many, and the first of their numbers.
    JS_EXPORT_PRIVATE ObjectID reserveObjectsOfInstances(uint32_t count);
    // By any thread, once for each of those numbers, before anybody names the object.
    JS_EXPORT_PRIVATE void defineObjectOfInstance(ObjectID, ObjectOfInstance);

    uint32_t size() const { return m_records.size(); }
    const ObjectRecord& at(ObjectID object) const { return m_records[object.index()]; }
    ObjectKind kindOf(ObjectID object) const { return at(object).kind; }
    LayoutID layoutOf(ObjectID object) const { return at(object).layout; }
    BuiltinClassID classOf(ObjectID object) const { return at(object).builtinClass; }
    OptionSet<ObjectTrait> traitsOf(ObjectID object) const { return at(object).traits; }

    static constexpr ObjectID unknown() { return ObjectID(0u); }
    JS_EXPORT_PRIVATE ObjectID objectOf(BuiltinID) const;
    JS_EXPORT_PRIVATE ObjectID objectOf(BuiltinID, ModuleID) const;
    // Every object that stands for the entry, ascending: objectOf(canonical), then its object in each module where modules have their own.
    template<size_t inlineCapacity>
    void appendObjectsOf(BuiltinID canonical, Vector<ObjectID, inlineCapacity>& objects) const
    {
        static_assert(ObjectKind::Builtin < ObjectKind::ModuleBuiltin);
        std::span<const ModuleBuiltin> all = m_moduleBuiltins.span();
        auto ofModules = std::ranges::equal_range(all, canonical, { }, &ModuleBuiltin::builtin);
        uint32_t first = firstOf(ObjectKind::ModuleBuiltin) + static_cast<uint32_t>(ofModules.begin() - all.begin());
        objects.reserveCapacity(objects.size() + 1 + ofModules.size());
        objects.append(objectOf(canonical));
        for (uint32_t index = 0; index < ofModules.size(); ++index)
            objects.append(ObjectID(first + index));
    }
    JS_EXPORT_PRIVATE ObjectID resultOf(BuiltinID, uint32_t ordinal) const;
    ObjectID hostObject(uint32_t indexOfRecord) const { return ObjectID(m_first[indexOf(ObjectKind::HostObject)] + indexOfRecord); }
    JS_EXPORT_PRIVATE ObjectID escapedFunctionsOf(BuiltinID holder) const;
    ObjectID escapedOf(BuiltinClassID builtinClass) const { return ObjectID(m_first[indexOf(ObjectKind::EscapedOfClass)] + builtinClass.index()); }
    ObjectID escapedWithoutShape(MakerOfShapelessObject maker) const { return ObjectID(m_first[indexOf(ObjectKind::EscapedWithoutShape)] + static_cast<uint32_t>(maker)); }
    ObjectID closureOf(FunctionID function) const { return ObjectID(m_first[indexOf(ObjectKind::Closure)] + m_placesByGroup[function.index()]); }
    ObjectID coroutineResultOf(FunctionID function) const { return ObjectID(m_first[indexOf(ObjectKind::CoroutineResult)] + m_placesWithCoroutinesFirst[function.index()]); }
    ObjectID restArrayOf(FunctionID function) const { return ObjectID(m_first[indexOf(ObjectKind::RestArray)] + m_placesWithRestArraysFirst[function.index()]); }
    ObjectID prototypeObjectOf(FunctionID function) const { return ObjectID(m_first[indexOf(ObjectKind::FunctionPrototype)] + m_placesByGroup[function.index()]); }
    ObjectID constructedObjectOf(FunctionID newTarget) const { return ObjectID(m_first[indexOf(ObjectKind::ConstructedObject)] + m_placesByGroup[newTarget.index()]); }
    ObjectID namespaceOf(ModuleID module) const { return ObjectID(m_first[indexOf(ObjectKind::Namespace)] + module.index()); }
    ObjectID instanceOf(LayoutID layout) const { return ObjectID(m_first[indexOf(ObjectKind::Instance)] + m_placesOfLayouts[layout.index()]); }
    ObjectID prototypeOf(LayoutID layout) const { return ObjectID(m_first[indexOf(ObjectKind::Prototype)] + m_placesOfLayouts[layout.index()]); }
    ObjectID escapedOf(LayoutID layout) const { return ObjectID(m_first[indexOf(ObjectKind::EscapedOfLayout)] + m_placesOfLayouts[layout.index()]); }
    // Null where the scan described no such site.
    JS_EXPORT_PRIVATE ObjectID siteObject(FunctionID, ValueID, uint32_t ordinal = 0) const;
    std::span<const ObjectRecord> sitesOf(FunctionID function) const LIFETIME_BOUND
    {
        return m_records.span().subspan(m_firstSiteOfFunction[function.index()], m_firstSiteOfFunction[function.index() + 1] - m_firstSiteOfFunction[function.index()]);
    }

    // The function of a closure, of an instance's too. Null for anything else.
    FunctionID closureFunction(ObjectID object) const { return at(object).kind == ObjectKind::Closure ? FunctionID(at(object).payload) : FunctionID(); }
    BuiltinID builtinOf(ObjectID object) const { return at(object).kind == ObjectKind::Builtin || at(object).kind == ObjectKind::ModuleBuiltin ? BuiltinID(at(object).payload) : BuiltinID(); }
    // Itself, or what it was copied from for an instance.
    ObjectID sharedFormOf(ObjectID object) const { return at(object).sharedForm ? at(object).sharedForm : object; }
    // What the object turns into once it has escaped. Itself where escaped objects of its kind stay apart: closures and whatever else a call enters, what is found by identity,
    // what rules and passes tell by its maker or by its record in the table, the families of instances and the prototype objects of functions,
    // and a built-in function whose call or read is an event (BuiltinTable::letsInUnseenCode(), raisesEventWhenCalled() of facts/AOTFlowRules.h, the loaders of machine code): what `unknown` covers is never one of those.
    // The native accessor that each module has one of turns into objectOf() of its entry, which stands for the one of whichever module. That object is no merged form: it keeps the rows of the entry.
    // Of a copy for an instance: the merged form of its layout, its class, its holder or its maker, as for its shared form.
    JS_EXPORT_PRIVATE ObjectID mergedFormOf(ObjectID) const;
    // For the reader of text. It looks at every object of instances.
    ObjectID findObjectOfInstance(ObjectOfInstance wanted) const
    {
        for (uint32_t index = firstObjectOfInstances(); index < size(); ++index) {
            if (m_records[index].instance == wanted.instance && m_records[index].sharedForm == wanted.sharedForm)
                return ObjectID(index);
        }
        return { };
    }

    uint32_t firstOf(ObjectKind kind) const { return m_first[indexOf(kind)]; }
    uint32_t endOf(ObjectKind kind) const { return m_first[indexOf(kind) + 1]; }
    uint32_t firstObjectOfInstances() const { return m_first[numberOfKinds]; }

private:
    static constexpr unsigned numberOfKinds = static_cast<unsigned>(ObjectKind::Site) + 1;
    static constexpr unsigned indexOf(ObjectKind kind) { return static_cast<unsigned>(kind); }
    std::optional<MakerOfShapelessObject> makerOfShapelessObjectThatMerges(ObjectID) const;

    struct ModuleBuiltin {
        BuiltinID builtin;
        ModuleID module;

        friend constexpr auto operator<=>(const ModuleBuiltin&, const ModuleBuiltin&) = default;
    };

    const BuiltinTable& m_builtins;
    std::array<uint32_t, numberOfKinds + 1> m_first { };
    // It grows between rounds alone, and room for the whole solve is not known: readers in a round hold no reference across a barrier.
    Vector<ObjectRecord> m_records;
    // By function: where its object stands among those of a kind.
    Vector<uint32_t> m_placesByGroup;
    Vector<uint32_t> m_placesWithCoroutinesFirst;
    Vector<uint32_t> m_placesWithRestArraysFirst;
    // By layout, the same.
    Vector<uint32_t> m_placesOfLayouts;
    Vector<uint32_t> m_firstSiteOfFunction;
    Vector<ModuleBuiltin> m_moduleBuiltins;
    // By canonical built-in: a function that no identity test finds and whose call or read is an event.
    BitVector m_builtinFunctionsThatStayApart;
    // By function: the body of a module of the library. What it makes may be what the module exports, which is found name by name.
    BitVector m_bodiesOfLibraryModules;
};

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
