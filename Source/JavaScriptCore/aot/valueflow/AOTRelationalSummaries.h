/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#if ENABLE(AOT)

#include "AOTIR.h"
#include "AOTVector.h"
#include <span>
#include <wtf/BitVector.h>
#include <wtf/Noncopyable.h>
#include <wtf/OptionSet.h>
#include <wtf/TZoneMalloc.h>

namespace JSC { namespace AOT {

class BuildContext;

namespace Typed {

class FlowResults;
class ProgramEditor;

enum class LinkedFunctions : uint8_t { All, ThoseThatTypeResults };

// What a function does with what it is handed, said once, in terms of its own parameters, and read at every call.

inline constexpr OptionSet<ValueFate> everyValueFate = OptionSet<ValueFate>::fromRaw((1u << 17) - 1);

// Somebody may hold the reference when the call has returned.
inline constexpr OptionSet<ValueFate> fatesThatKeep {
    ValueFate::IsReturned,
    ValueFate::IsThrown,
    ValueFate::IsYielded,
    ValueFate::IsStoredInVariable,
    ValueFate::IsStoredInEnvironment,
    ValueFate::IsStoredInUnknownObject,
    ValueFate::IsHandedToUnknownCode,
    ValueFate::IsIncomplete,
};

// An access to a member finds an accessor, a protocol looks for a method: with the object as `this`. Whether one is there depends on the object, which the caller knows.
inline constexpr OptionSet<ValueFate> fatesThatMayRunCodeOfTheObject {
    ValueFate::HasMembersRead,
    ValueFate::HasMembersWritten,
    ValueFate::HasMembersDeletedOrDefined,
    ValueFate::HasKeysEnumerated,
    ValueFate::IsIterated,
    ValueFate::IsConvertedToPrimitive,
};

// Ascending. More than a few count as any.
struct KeysOfMembers {
    static constexpr unsigned mostKeys = 8;

    static KeysOfMembers any() { return { { }, true }; }
    bool isEmpty() const { return !mayBeAnyKey && keys.isEmpty(); }
    // Both answer whether something was added.
    bool add(PropertyKeyID);
    bool unite(const KeysOfMembers&);

    Vector<PropertyKeyID, 4> keys;
    bool mayBeAnyKey { false };

    friend bool operator==(const KeysOfMembers&, const KeysOfMembers&) = default;
};

// Of an entry parameter, or of an operand of one call: all that the callee, and whatever the callee hands it on to, does with it.
struct LinkedFate {
    static LinkedFate anything() { return { }; }
    static LinkedFate nothing() { return { { }, { }, 0, 0, false, 0, { }, { } }; }
    bool isAnything() const { return itself.contains(ValueFate::IsIncomplete); }
    // Answers whether something was added.
    bool unite(const LinkedFate&);

    OptionSet<ValueFate> itself { everyValueFate };
    // The same of every reference that is read through it. What a closure has captured is among its contents.
    OptionSet<ValueFate> contents { everyValueFate };
    // Bit k: it may be result k.
    uint32_t results { UINT32_MAX };
    // Bit j: it may be stored into a member of entry parameter j, or of operand j of the call, or of something read through that.
    uint32_t storedIntoParameters { UINT32_MAX };
    // False: every such store went into a member of the parameter itself, `t.f = v`, and none into something that was read through it, `t.slot.held = v`.
    bool mayBeStoredDeeper { true };
    // Bit k: it may be stored into a member of result k, or of something read through that. It lives as long as the result, and whoever reads it out of the result has it.
    uint32_t storedIntoResults { UINT32_MAX };
    KeysOfMembers keysRead { KeysOfMembers::any() };
    KeysOfMembers keysWritten { KeysOfMembers::any() };

    friend bool operator==(const LinkedFate&, const LinkedFate&) = default;
};

// What FlowResults::fateOfOperandAt() answers, of the operand with that index of one operation.

// Nobody holds the reference any more when the call has returned, provided that no access of the callee runs code of the object: see runsNoCodeOn().
inline bool isKeptNowhere(const LinkedFate& fate) { return !fate.itself.containsAny(fatesThatKeep) && !fate.storedIntoParameters && !fate.storedIntoResults; }
// The same of all that is read through it, provided that no access runs code of an object among those. No list of keys says which accesses there are.
inline bool keepsContentsNowhere(const LinkedFate& fate) { return !fate.contents.containsAny(fatesThatKeep); }
inline bool mayRunCodeOfContents(const LinkedFate& fate) { return fate.contents.containsAny(fatesThatMayRunCodeOfTheObject); }

// One of the values that a result may be, in terms of the function that was asked about.
struct LinkedOrigin {
    enum class Kind : uint8_t {
        // `parameter`: that entry parameter.
        Parameter,
        // The operation that makes the object: `opcode`, `type` as the operation was typed when the link ran, `allocationSite` as flow has stamped it.
        Allocation,
        // The literal: `opcode`, and `type`, which holds that one value where the type table can say so. Without `value`: the undefined of `return;`, or of an argument that a call does not pass.
        Constant,
        // `opcode`: an operation of which the IR says what it answers: an operator, a test, a template.
        Computed,
        // What the operation `opcode` has read out of the origin `source`: under `key`, out of `field` (a field of an environment, a private field), or under a key that was computed.
        ReadFrom,
        // What a call of the built-in function `builtin` has answered, or a construction (`isConstruction`).
        BuiltinResult,
        // What a call of a value has answered of which the function cannot say what it enters, or a construction (`isConstruction`).
        CallResult,
        // Any of its operands. It stands where an operand or a source may be one of several values, and is never among `mayBe`.
        OneOf,
        // A value that nobody can list: `x.length > limit` is a boolean whatever the variable `limit` holds.
        Anything,
    };

    // The table holds one of these for each value that a result of the program may be, to the end of the build: the fields are as narrow as the limits of the link allow.
    Kind kind { Kind::Parameter };
    // Of BuiltinResult and CallResult.
    bool isConstruction { false };
    // Of Allocation: nobody but the result holds the object when the call returns.
    bool isHeldByNobodyElse { false };
    // Of Allocation: the stores are all that has been written to the object. Nothing was defined, deleted or enumerated on it, and no access to it has run code.
    bool hasOnlyListedStores { false };
    // Of Allocation: the way down is not listed. It leads through a function that calls itself, or through too many calls.
    bool hasUnlistedCalls { false };
    uint8_t source { 0 };
    Opcode opcode { Opcode::Undefined };
    TypeID type;
    uint32_t parameter { 0 };
    FunctionID function;
    ValueID value;
    AllocationSiteID allocationSite;
    PropertyKeyID key;
    FieldReference field;
    BuiltinID builtin;
    uint16_t firstOperand { 0 };
    uint16_t firstCall { 0 };
    uint8_t operandCount { 0 };
    uint8_t callCount { 0 };
    uint8_t firstStore { 0 };
    uint8_t storeCount { 0 };

    friend bool operator==(const LinkedOrigin&, const LinkedOrigin&) = default;
};

// The call of `function` whose result is the object. `site` is flow's stamp, which the call keeps whatever a pass turns it into.
struct CallOnTheWay {
    FunctionID function;
    ValueID call;
    AccessSiteID site;

    friend bool operator==(const CallOnTheWay&, const CallOnTheWay&) = default;
};

// The origin with the index `origin` was stored into the object, under `key` or under a key that was computed.
struct StoreIntoOrigin {
    PropertyKeyID key;
    // Whichever way the call went, the operation ran. With hasOnlyListedStores: the member is there.
    bool isOnEveryPath { false };
    // The operation that makes the object puts the value in: the member is there from birth.
    bool isAtCreation { false };
    uint8_t origin { 0 };

    friend bool operator==(const StoreIntoOrigin&, const StoreIntoOrigin&) = default;
};

struct ResultOrigin {
    // Where the link starts inside a cycle of calls. What is listed only grows from here, and the two flags only fall.
    static ResultOrigin nothingYet()
    {
        ResultOrigin origin;
        origin.isFresh = true;
        origin.isFullyListed = true;
        origin.isFreshWhenConstructed = true;
        return origin;
    }

    // Every call answers an object that this activation, or one below it, has made and that nobody else holds: all of `mayBe` are allocations that say isHeldByNobodyElse.
    bool isFresh { false };
    // The result is one of `mayBe` and nothing else. Nothing listed: no call returns.
    bool isFullyListed { false };
    // Of a constructor function: `new f(..)`, with the function itself as new.target, answers the object that was made for the construction, and nobody else holds it.
    bool isFreshWhenConstructed { false };
    // How often the link has changed the answer on its way around a cycle of calls. An answer that does not come to rest becomes "anything".
    uint8_t numberOfChanges { 0 };
    // Indexes into `origins`, ascending.
    Vector<uint8_t> mayBe;
    // At most 32. The operands and the source of an origin stand in front of it, so that one loop from the front types them all. A store may name any of them.
    Vector<LinkedOrigin> origins;
    Vector<uint8_t> operands;
    Vector<CallOnTheWay> calls;
    Vector<StoreIntoOrigin> stores;

    void dump(PrintStream&) const;

    friend bool operator==(const ResultOrigin&, const ResultOrigin&) = default;
};

// Where a value stands at a call by value, whichever operation makes the call. It outlives the passes, which renumber values and turn one call operation into another.
struct PlaceAtCall {
    enum class Kind : uint8_t { Callee, This, NewTarget, Argument, ElementOfList };

    Kind kind { Kind::Argument };
    bool isConstruction { false };
    uint32_t argument { 0 };

    friend bool operator==(const PlaceAtCall&, const PlaceAtCall&) = default;
};

// The function, or one that it hands both on to, calls whatever its entry parameter `called` holds and passes its entry parameter `handed`, or something read through that, at `place`.
struct HandedToCalledParameter {
    uint32_t called { 0 };
    uint32_t handed { 0 };
    bool isReadThroughHanded { false };
    FunctionID function;
    ValueID call;
    PlaceAtCall place;
    // All that the calling function, and those above it up to the function that was asked about, do with what the call answers and with what is read through that.
    OptionSet<ValueFate> resultOfCall { everyValueFate };

    friend bool operator==(const HandedToCalledParameter&, const HandedToCalledParameter&) = default;
};

class RelationalSummaries {
    WTF_MAKE_TZONE_ALLOCATED(RelationalSummaries);
    WTF_MAKE_NONCOPYABLE(RelationalSummaries);
public:
    RelationalSummaries();
    ~RelationalSummaries();

    // By function as read and entry parameter as read.
    const LinkedFate& fateOfParameter(FunctionID, uint32_t entryParameter) const LIFETIME_BOUND;
    // The same without what the values do that the function is handed and calls: fateOfParameter() takes those for unknown code. Each such call has a row in the list.
    const LinkedFate& fateApartFromCalledParameters(FunctionID, uint32_t entryParameter) const LIFETIME_BOUND;
    std::span<const HandedToCalledParameter> handedToCalledParameters(FunctionID) const LIFETIME_BOUND;
    const ResultOrigin& originOfResult(FunctionID, uint32_t result) const LIFETIME_BOUND;
    // A pass has taken entry parameters away from the function itself.
    void removeEntryParameters(FunctionID, const BitVector& removedEntryParameters);

    void dump(PrintStream&, const Program&) const;

private:
    friend std::unique_ptr<RelationalSummaries> linkRelationalSummaries(const Program&, const FlowResults&, BuildContext&, LinkedFunctions);

    struct OfFunction {
        uint32_t numberOfEntryParameters { 0 };
        Vector<LinkedFate> parameters;
        // Empty where it would say the same as `parameters`. So is `parameters` where every entry would say "anything".
        Vector<LinkedFate> parametersApartFromCalledParameters;
        Vector<HandedToCalledParameter> handedToCalledParameters;
        // There were more rows than are kept. Then there is none, and the part apart is the whole.
        bool hasTooManyRows { false };
        Vector<ResultOrigin> results;
    };

    // While the table is being linked, functions are visited side by side.
    struct Order {
        Vector<uint32_t> levelOf;
        Vector<uint32_t> componentOf;
    };
    bool isKnownToAsker(FunctionID) const;
    static void setAsker(uint32_t level, uint32_t component);

    Vector<OfFunction> m_functions;
    const Order* m_orderWhileLinking { nullptr };
    LinkedFate m_anything;
    ResultOrigin m_unknownOrigin;
};

// Reads the records of the functions and, at a call by value, the callees that flow lists. Defined in AOTRelationalSummariesLink.cpp.
std::unique_ptr<RelationalSummaries> linkRelationalSummaries(const Program&, const FlowResults&, BuildContext&, LinkedFunctions = LinkedFunctions::All);
void validateResultOriginsAtRunTime(ProgramEditor&, const Program&, const FlowResults&, BuildContext&);
// The front end gives no site to a result that is declared as an interface of the library (`[Symbol.iterator](): Iterator<number>`), so flow publishes nothing for the position and it stays expected.
void typeResultsByOrigins(Program&, const FlowResults&, BuildContext&);
bool mayBeTypedByOrigins(const Function&);

// Whether none of the accesses that the fate lists can run code when they meet an object of the type: each finds an own field that is filled, or ends on a chain that holds no accessor under the key and that nobody can change.
bool runsNoCodeOn(const Program&, const FlowResults&, const Type& typeOfObject, const LinkedFate&);
// Whether each store that the fate lists puts the value into an own data property of the object.
bool storesLandInOwnFields(const Program&, const FlowResults&, const Type& typeOfObject, const LinkedFate&);
// The same of an argument, `this` or new.target of one call.
bool runsNoCodeOnOperandAt(const Program&, const FlowResults&, const Function& caller, const Value& call, uint32_t operandIndex, const LinkedFate&);

// What flow has proven of the value of an operation, beyond the type that the operation has by its rule, stands in a cast right behind the operation.
class ProofsBehindValues {
public:
    explicit ProofsBehindValues(const Function&);
    // None: there is no such cast.
    TypeID find(ValueID) const;

private:
    struct Proof {
        ValueID value;
        TypeID type;
    };

    // Ascending by value.
    Vector<Proof> m_proofs;
};

// The same of the object that an operation of the function makes, or that a call of it answers, with all that the function and whatever it hands the object to do with it.
bool runsNoCodeOnObjectMadeBy(const Program&, const FlowResults&, const Function&, const ProofsBehindValues&, ValueID allocationOrCall, const LinkedFate&);

// Ascending.
using GettersOfProgram = Vector<FunctionID, 2>;
// The getters of the program that the accesses may run with the object as `this`, where they can run nothing else: getters that a class defines for good, found by a read under a listed key.
std::optional<GettersOfProgram> gettersRunOn(const Program&, const FlowResults&, const Type& typeOfObject, const LinkedFate&);
std::optional<GettersOfProgram> gettersRunOnOperandAt(const Program&, const FlowResults&, const Function& caller, const Value& call, uint32_t operandIndex, const LinkedFate&);
std::optional<GettersOfProgram> gettersRunOnObjectMadeBy(const Program&, const FlowResults&, const Function&, const ProofsBehindValues&, ValueID allocationOrCall, const LinkedFate&);

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
