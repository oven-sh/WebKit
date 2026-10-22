/*
 * Copyright (C) 2026 Oven, Inc. All rights reserved.
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "config.h"
#include "AOTRelationalSummaries.h"

#if ENABLE(AOT)

#include "AOTBuildContext.h"
#include "AOTBuiltinIDs.h"
#include "AOTCFG.h"
#include "AOTFlowResults.h"
#include "AOTRelationalSummariesInternal.h"
#include "AOTStronglyConnectedComponents.h"
#include "AOTThreads.h"
#include <algorithm>
#include <wtf/DataLog.h>
#include <wtf/HashMap.h>

namespace JSC { namespace AOT { namespace Typed {

namespace {

// The record of one function, as the front end lays it down, without what the fates do not need.
using SubjectKind = RelationSubjectKind;
using Subject = RelationSubject;

struct Store {
    uint32_t source { 0 };
    uint32_t target { 0 };
    PropertyKeyID key;
    // The target is made with the value.
    bool isCreationOperand { false };
};

struct Handing {
    uint32_t source { 0 };
    ValueID call;
    uint32_t operandIndex { 0 };
};

struct Record {
    Vector<Subject> subjects;
    Vector<Store> stores;
    Vector<Handing> handed;
};

constexpr uint32_t noSubject = UINT32_MAX;
// What the front end lists at most.
constexpr unsigned mostSubjects = 4096;

// Subjects of one record. Nine records of ten have at most 64: one word.
class SetOfSubjects {
public:
    bool contains(uint32_t subject) const { return subject >> 6 < m_words.size() && (m_words[subject >> 6] >> (subject & 63) & 1); }
    // Whether it was not there.
    bool add(uint32_t subject)
    {
        while (m_words.size() <= subject >> 6)
            m_words.append(0);
        uint64_t bit = 1ull << (subject & 63);
        if (m_words[subject >> 6] & bit)
            return false;
        m_words[subject >> 6] |= bit;
        return true;
    }
    // The functor may add to the set. What it adds in front of where the loop stands is not visited.
    void forEach(NOESCAPE const Invocable<void(uint32_t)> auto& functor) const
    {
        for (size_t word = 0; word < m_words.size(); ++word) {
            for (uint64_t rest = m_words[word]; rest; rest &= rest - 1)
                functor(static_cast<uint32_t>(word << 6 | std::countr_zero(rest)));
        }
    }

private:
    Vector<uint64_t, 1> m_words;
};

// No subject: nothing is known of the function. A record that names what is not there says nothing either.
std::optional<Record> recordOf(const Function& function)
{
    const FunctionRelations& relations = function.relations;
    uint32_t numberOfSubjects = relations.subjects.size();
    if (!numberOfSubjects || numberOfSubjects > mostSubjects)
        return std::nullopt;
    // The activation of a coroutine outlives the call, with all that it was handed, whatever the record says of the body.
    if (function.isUnsplitCoroutine())
        return std::nullopt;
    uint32_t numberOfParameters = function.at(entryBlock).parameters.size();
    BitVector listedParameters(numberOfParameters);
    for (uint32_t index = 0; index < numberOfSubjects; ++index) {
        const RelationSubject& subject = relations.subjects[index];
        bool namesValue = subject.kind != RelationSubjectKind::Parameter && !(subject.kind == RelationSubjectKind::Constant && subject.index == UINT32_MAX);
        if ((subject.kind == RelationSubjectKind::Parameter && subject.index >= numberOfParameters) || (namesValue && subject.index >= function.values.size()))
            return std::nullopt;
        // Each entry parameter is listed once, and a chain of reads begins somewhere.
        if (subject.kind == RelationSubjectKind::Parameter && listedParameters.set(subject.index))
            return std::nullopt;
        uint32_t root = index;
        for (uint32_t steps = 0; relations.subjects[root].kind == RelationSubjectKind::ReadFrom; ++steps) {
            root = relations.subjects[root].source;
            if (root >= numberOfSubjects || steps == numberOfSubjects)
                return std::nullopt;
        }
    }

    Record record;
    record.subjects = relations.subjects;
    // A row without a subject says that something unknown stands there. Whoever looks for a subject there and finds none takes it for that.
    for (const StoredInto& store : relations.stores) {
        if (store.target == noSubject)
            continue;
        if ((store.source != noSubject && store.source >= numberOfSubjects) || store.target >= numberOfSubjects)
            return std::nullopt;
        record.stores.append(Store { store.source, store.target, store.key, !!store.creationOperand });
    }
    for (const HandedTo& handed : relations.handed) {
        if (handed.source == noSubject)
            continue;
        if (handed.source >= numberOfSubjects || handed.call >= function.values.size())
            return std::nullopt;
        const Value& call = function.at(ValueID(handed.call));
        bool entersCode = Value::opcodesWithCallees.contains(call.opcode) || call.opcode == Opcode::NewClosure || call.opcode == Opcode::CallDirect;
        if (!entersCode || handed.argument >= call.numberOfOperands())
            return std::nullopt;
        record.handed.append(Handing { handed.source, ValueID(handed.call), handed.argument });
    }
    // The rows of one call stand together, in the order in which they were listed.
    std::ranges::stable_sort(record.handed, { }, [](const Handing& handing) {
        return handing.call.index();
    });
    return record;
}
constexpr unsigned mostRows = 16;
// A call that may enter more functions than this is a call of nobody knows whom. Where all the modules of a bundled file share one object for their exports, each of N calls lists N callees.
constexpr unsigned mostCalleesFollowed = 256;

CalleeList calleesFollowedAt(const Program& program, const Value& call)
{
    CalleeList callees = calleesOfCall(program, call);
    if (callees.functions.size() + callees.builtins.size() > mostCalleesFollowed)
        return { };
    return callees;
}

// All that is done with a value and with what is read through it, without the masks.
OptionSet<ValueFate> withoutMasks(const LinkedFate& fate)
{
    OptionSet<ValueFate> fates = fate.itself | fate.contents;
    if (fate.storedIntoParameters || fate.storedIntoResults)
        fates.add(ValueFate::IsStoredInUnknownObject);
    return fates;
}

// All the operands behind the named ones share what the schema declares for the first of them.
bool isCalledBy(const Value& call, uint32_t operandIndex)
{
    return declaredBehaviorOf(call.opcode, std::min(operandIndex, numberOfNamedOperandsOf(call.opcode))).uses.containsAny(callingUses);
}

// The same row but for what becomes of the result, which only grows.
bool isSameCall(const HandedToCalledParameter& a, const HandedToCalledParameter& b)
{
    return a.called == b.called && a.handed == b.handed && a.isReadThroughHanded == b.isReadThroughHanded && a.function == b.function && a.call == b.call && a.place == b.place;
}

uint32_t bitOfParameter(uint32_t parameter)
{
    return parameter < 32 ? 1u << parameter : 0;
}

// What is known of a subject so far. `whole` takes a value that the function is handed and calls for unknown code. `apart` leaves out what such a value does: a row says that it is called.
struct StateOfSubject {
    LinkedFate whole { LinkedFate::nothing() };
    LinkedFate apart { LinkedFate::nothing() };
    // Grows with every addition to what is known of the subject in a visit.
    uint32_t version { 0 };
    // None before askOwnObjects() has looked at the subject.
    uint32_t versionWhenAsked { UINT32_MAX };
};

struct StateOfHanding {
    // The subject that the call answers, if it is one.
    uint32_t answer { noSubject };
    // The version of that subject when the row was last followed. None before the row has been followed.
    uint32_t versionWhenFollowed { UINT32_MAX };
};

class FunctionLinker {
public:
    FunctionLinker(const RelationalLink& link, FunctionID id, const Record& record, bool hadTooManyRows)
        : m_link(link)
        , m_id(id)
        , m_function(link.program.at(id))
        , m_record(record)
        , m_states(record.subjects.size())
        , m_roots(record.subjects.size())
    {
        RELEASE_ASSERT(record.subjects.size() <= mostSubjects);
        m_objectsLivedIn.grow(record.subjects.size());
        m_objectsLivedDeepIn.grow(record.subjects.size());
        m_subjectsLivingIn.grow(record.subjects.size());
        for (uint32_t index = 0; index < record.subjects.size(); ++index) {
            if (record.subjects[index].kind == SubjectKind::Allocation || record.subjects[index].kind == SubjectKind::CallResult)
                m_subjectsOfValues.append({ record.subjects[index].index, index });
        }
        std::ranges::sort(m_subjectsOfValues);
        m_closuresCapturedBy.grow(record.subjects.size());
        m_hasTooManyRows = hadTooManyRows;
        m_rowStates.grow(record.handed.size());
        for (uint32_t row = 0; row < record.handed.size(); ++row)
            m_rowStates[row].answer = findSubjectOf(record.handed[row].call, SubjectKind::CallResult);
    }

    void run();

    LinkedFate fateOfParameter(uint32_t entryParameter, bool apartFromCalledParameters) const;
    Vector<LinkedFate> fatesOfSubjects() const;
    Vector<HandedToCalledParameter> takeRows() { return WTF::move(m_rows); }
    bool hasTooManyRows() const { return m_hasTooManyRows; }
    void addRowsOfLastVisit(std::span<const HandedToCalledParameter>);

private:
    enum class Part : uint8_t { Whole, Apart };
    static constexpr std::array bothParts { Part::Whole, Part::Apart };

    const Subject& subject(uint32_t index) const { return m_record.subjects[index]; }
    LinkedFate& fate(uint32_t index, Part part) { return part == Part::Whole ? m_states[index].whole : m_states[index].apart; }
    bool isReadThroughRoot(uint32_t index) const { return subject(index).kind == SubjectKind::ReadFrom; }

    void findRoots();
    void start();
    void didChange(uint32_t index);
    void unite(uint32_t index, Part, const LinkedFate& other);
    void addToItself(uint32_t index, Part, OptionSet<ValueFate>);
    void becomeAnything(uint32_t index, Part);
    void becomeAnything(uint32_t index, std::span<const Part>);
    void becomeAnything(uint32_t index);

    void followReads();
    void noteStore(uint32_t source, uint32_t target, bool mayLieDeeper);
    void closeStores();
    void followStores();
    void askOwnObjects();
    void followStore(uint32_t source, uint32_t root, Part);
    enum class ReadOutAgain : uint8_t { No, Yes };
    void liveInside(uint32_t source, uint32_t object, Part, ReadOutAgain = ReadOutAgain::Yes);
    void beReadOutOf(uint32_t source, uint32_t object, Part);
    bool isFreshObject(uint32_t callResult);
    void followCapture(const Handing&, const Value& closure);
    OptionSet<ValueFate> fatesOfResultOf(ValueID call, Part);
    void followHanding(const Handing&);
    const CalleeList& calleesOf(ValueID, const Value& call) LIFETIME_BOUND;
    void followCallOf(const Handing&, const Value& call, const CalleeList&);
    void applyCallee(const Handing&, const Value& call, FunctionID callee);
    void applyFateOfCallee(const Handing&, const LinkedFate&, Part, NOESCAPE const Invocable<bool(uint32_t operandIndex, uint32_t bit)> auto& isOperandOfBit);
    void applyRowsOfCallee(const Handing&, const Value& call, FunctionID callee, uint32_t entryParameter, bool isElementOfParameter);
    struct AnswerOfBuiltin {
        uint32_t row { 0 };
        BuiltinID builtin;
        LinkedFate withCallbacks;
        LinkedFate withoutCallbacks;
        Vector<CallbackOfBuiltin, 2> callbacks;
    };
    void applyBuiltin(const Handing&, const Value& call, BuiltinID);
    const AnswerOfBuiltin& answerOf(const Handing&, const Value& call, BuiltinID) LIFETIME_BOUND;
    void composeWithCalledValue(const Handing&, uint32_t calledSubject, const HandedToCalledParameter& row, std::span<const Part>);
    bool isTakenAs(const Function& callee, const Value& call, uint32_t operandIndex, uint32_t entryParameter) const;
    void noteCallOfParameter(const Handing&, uint32_t calledParameter);
    uint32_t findCallResult(ValueID call, uint32_t result) const;
    // The subject of that kind whose value it is.
    uint32_t findSubjectOf(ValueID, SubjectKind) const;
    std::span<const Handing> handedAt(ValueID call) const LIFETIME_BOUND;
    // The entry parameter that the call calls, if what it calls is an entry parameter of this function as it came in.
    std::optional<uint32_t> parameterCalledBy(ValueID call, const Value&) const;
    void addRow(HandedToCalledParameter&&);

    const RelationalLink& m_link;
    FunctionID m_id;
    const Function& m_function;
    const Record& m_record;
    Vector<StateOfSubject> m_states;
    // The subject that a chain of reads starts at. A subject that is read out of nothing is its own root.
    Vector<uint32_t> m_roots;
    // Bit r: the subject has been stored into a member of the root r, or of something that is read through r. In the second list only the latter.
    Vector<SetOfSubjects> m_objectsLivedIn;
    Vector<SetOfSubjects> m_objectsLivedDeepIn;
    // By root: the subjects in whose first list it stands.
    Vector<Vector<uint32_t, 2>> m_subjectsLivingIn;
    // Allocations and what calls answer: (value, subject), ascending.
    Vector<std::pair<uint32_t, uint32_t>> m_subjectsOfValues;
    // Bit c: the closure c, which this function makes, has captured the subject.
    Vector<SetOfSubjects> m_closuresCapturedBy;
    // Bit r: the root r, or something that is read through it, may hold an object that is no subject: a value that the record does not follow, or one that a callee has stored.
    SetOfSubjects m_rootsHoldingUnlistedValues;
    std::array<SetOfSubjects, 2> m_resultsWhoseCodeMayRun;
    std::optional<ProofsBehindValues> m_proofs;
    Vector<HandedToCalledParameter> m_rows;
    // A list stays where it is while others are added.
    HashMap<uint32_t, std::unique_ptr<CalleeList>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> m_calleesOfCalls;
    Vector<AnswerOfBuiltin> m_answersOfBuiltins;
    // By row of m_record.handed.
    Vector<StateOfHanding, 16> m_rowStates;
    bool m_hasChanged { false };
    bool m_hasTooManyRows { false };
};

void FunctionLinker::findRoots()
{
    // recordOf() has seen that every chain begins somewhere.
    for (uint32_t index = 0; index < m_record.subjects.size(); ++index) {
        uint32_t root = index;
        while (subject(root).kind == SubjectKind::ReadFrom)
            root = subject(root).source;
        m_roots[index] = root;
    }
}

void FunctionLinker::start()
{
    for (uint32_t index = 0; index < m_record.subjects.size(); ++index) {
        const Subject& listed = subject(index);
        for (Part part : bothParts) {
            LinkedFate& known = fate(index, part);
            if (listed.fate.contains(ValueFate::IsIncomplete)) {
                known = LinkedFate::anything();
                continue;
            }
            known.itself = listed.fate;
            // Who converts an array converts its elements.
            if (listed.fate.contains(ValueFate::IsConvertedToPrimitive))
                known.contents.add(ValueFate::IsConvertedToPrimitive);
            if (listed.fate.contains(ValueFate::IsReturned))
                known.results = listed.results ? listed.results : UINT32_MAX;
        }
    }
    SetOfSubjects subjectsWithListedReads;
    SetOfSubjects subjectsWithListedStores;
    for (uint32_t index = 0; index < m_record.subjects.size(); ++index) {
        if (subject(index).kind == SubjectKind::ReadFrom)
            subjectsWithListedReads.add(subject(index).source);
    }
    PropertyKeyID length = m_link.program.keyOf(BuiltinNames::length);
    for (const Store& store : m_record.stores) {
        // `a.length = v`: an array makes a number of v, which runs valueOf | toString of v, and holds the number. Nobody here knows whether the target is an array.
        if (store.key && store.key == length && store.source != noSubject) {
            for (Part part : bothParts) {
                fate(store.source, part).itself.add(ValueFate::IsConvertedToPrimitive);
                fate(store.source, part).contents.add(ValueFate::IsConvertedToPrimitive);
            }
        }
        subjectsWithListedStores.add(store.target);
        // What an object is made with is put there by no store: no lookup, no setter, under no key that anybody could ask about.
        if (store.isCreationOperand)
            continue;
        for (Part part : bothParts) {
            KeysOfMembers& written = fate(store.target, part).keysWritten;
            if (store.key)
                written.add(store.key);
            else
                written = KeysOfMembers::any();
        }
    }
    // The front end lists every read of a member and every store into one. A flag without a row is an access under a key that nobody knows.
    for (uint32_t index = 0; index < m_record.subjects.size(); ++index) {
        for (Part part : bothParts) {
            if (subject(index).fate.contains(ValueFate::HasMembersRead) && !subjectsWithListedReads.contains(index))
                fate(index, part).keysRead = KeysOfMembers::any();
            // Something was taken out of it, and nobody has followed what: a step of a built-in iterator, an internal field.
            if (subject(index).fate.containsAny({ ValueFate::HasMembersRead, ValueFate::IsIterated, ValueFate::HasKeysEnumerated }) && !subjectsWithListedReads.contains(index))
                fate(index, part).contents = everyValueFate;
            if (subject(index).fate.contains(ValueFate::HasMembersWritten) && !subjectsWithListedStores.contains(index))
                fate(index, part).keysWritten = KeysOfMembers::any();
        }
    }
}

void FunctionLinker::didChange(uint32_t index)
{
    ++m_states[index].version;
    m_hasChanged = true;
}

void FunctionLinker::unite(uint32_t index, Part part, const LinkedFate& other)
{
    if (fate(index, part).unite(other))
        didChange(index);
}

void FunctionLinker::addToItself(uint32_t index, Part part, OptionSet<ValueFate> fates)
{
    LinkedFate& known = fate(index, part);
    if (known.itself.containsAll(fates))
        return;
    known.itself.add(fates);
    didChange(index);
}

void FunctionLinker::becomeAnything(uint32_t index, Part part)
{
    LinkedFate& known = fate(index, part);
    if (known.isAnything())
        return;
    known = LinkedFate::anything();
    didChange(index);
}

void FunctionLinker::becomeAnything(uint32_t index, std::span<const Part> parts)
{
    for (Part part : parts)
        becomeAnything(index, part);
}

void FunctionLinker::becomeAnything(uint32_t index)
{
    becomeAnything(index, bothParts);
}

// What becomes of a value that was read out of an object is what becomes of the contents of the object.
void FunctionLinker::followReads()
{
    for (uint32_t index = m_record.subjects.size(); index--;) {
        const Subject& read = subject(index);
        if (read.kind != SubjectKind::ReadFrom)
            continue;
        for (Part part : bothParts) {
            const LinkedFate& ofRead = fate(index, part);
            LinkedFate ofSource = LinkedFate::nothing();
            ofSource.contents = withoutMasks(ofRead);
            if (read.key)
                ofSource.keysRead.add(read.key);
            else
                ofSource.keysRead = KeysOfMembers::any();
            unite(read.source, part, ofSource);
        }
    }
}

// From here on the value lies in a member of the target.
void FunctionLinker::noteStore(uint32_t source, uint32_t target, bool mayLieDeeper)
{
    uint32_t root = m_roots[target];
    mayLieDeeper |= root != target;
    bool isNew = m_objectsLivedIn[source].add(root);
    if (isNew)
        m_subjectsLivingIn[root].append(source);
    if (mayLieDeeper)
        isNew |= m_objectsLivedDeepIn[source].add(root);
    m_hasChanged |= isNew;
}

// `temp.slot = k; temp.slot.held = v`: a store through a read lands in what the object holds, not in the object. What is read out of an object may be whatever has been stored into it.
void FunctionLinker::closeStores()
{
    for (uint32_t source = 0; source < m_record.subjects.size(); ++source) {
        m_objectsLivedDeepIn[source].forEach([&](uint32_t root) {
            if (m_rootsHoldingUnlistedValues.contains(root)) {
                for (Part part : bothParts)
                    addToItself(source, part, ValueFate::IsStoredInUnknownObject);
            }
            // noteStore() may append to the list.
            for (size_t position = 0; position < m_subjectsLivingIn[root].size(); ++position) {
                if (uint32_t held = m_subjectsLivingIn[root][position]; held != source)
                    noteStore(source, held, true);
            }
        });
    }
}

void FunctionLinker::followStores()
{
    for (uint32_t source = 0; source < m_record.subjects.size(); ++source) {
        m_objectsLivedIn[source].forEach([&](uint32_t root) {
            for (Part part : bothParts)
                followStore(source, root, part);
        });
        // Nobody but its own function reads what a closure has captured, and followCapture() has asked that.
        m_closuresCapturedBy[source].forEach([&](uint32_t closure) {
            for (Part part : bothParts)
                liveInside(source, closure, part, ReadOutAgain::No);
        });
    }
}

void FunctionLinker::followStore(uint32_t source, uint32_t root, Part part)
{
    switch (subject(root).kind) {
    case SubjectKind::Parameter: {
        LinkedFate stored = LinkedFate::nothing();
        stored.storedIntoParameters = bitOfParameter(subject(root).index);
        stored.mayBeStoredDeeper = m_objectsLivedDeepIn[source].contains(root);
        if (!stored.storedIntoParameters)
            stored.itself.add(ValueFate::IsStoredInUnknownObject);
        unite(source, part, stored);
        // `temp.slot = k; kept.push(temp.slot)`: what this function does with what it reads out of the parameter, it may do with the value.
        beReadOutOf(source, root, part);
        return;
    }
    case SubjectKind::Allocation:
        liveInside(source, root, part);
        return;
    case SubjectKind::CallResult:
        // `const t = make(); t.f = v`: an object that a callee has made for this call and that nobody else holds is as good as one that was made here.
        if (isFreshObject(root)) {
            liveInside(source, root, part);
            return;
        }
        [[fallthrough]];
    case SubjectKind::ReadFrom:
    case SubjectKind::Constant:
    case SubjectKind::Computed:
        // Somebody else may hold the object.
        addToItself(source, part, ValueFate::IsStoredInUnknownObject);
        return;
    }
    RELEASE_ASSERT_NOT_REACHED();
}

// Whether every function that the call may enter answers an object that it has made for the call, or one below it has, and that nobody else holds.
bool FunctionLinker::isFreshObject(uint32_t callResult)
{
    ValueID id(subject(callResult).index);
    const CalleeList& callees = calleesOf(id, m_function.at(id));
    if (!callees.isComplete || !callees.builtins.isEmpty() || callees.functions.isEmpty())
        return false;
    return std::ranges::all_of(callees.functions, [&](FunctionID callee) {
        return m_link.linked.originOfResult(callee, 0).isFresh;
    });
}

// The value lives as long as the object and goes where that goes. Whoever reads it out of the object does with it what is done with the contents of the object.
void FunctionLinker::liveInside(uint32_t source, uint32_t object, Part part, ReadOutAgain readOutAgain)
{
    const LinkedFate& ofObject = fate(object, part);
    if (ofObject.isAnything() || m_resultsWhoseCodeMayRun[static_cast<unsigned>(part)].contains(object)) {
        becomeAnything(source, part);
        return;
    }
    LinkedFate stored = LinkedFate::nothing();
    stored.itself = ofObject.itself & fatesThatKeep;
    stored.itself.remove(ValueFate::IsReturned);
    stored.storedIntoResults = ofObject.results | ofObject.storedIntoResults;
    stored.storedIntoParameters = ofObject.storedIntoParameters;
    stored.mayBeStoredDeeper = ofObject.storedIntoParameters;
    if (readOutAgain == ReadOutAgain::No) {
        unite(source, part, stored);
        return;
    }
    unite(source, part, stored);
    beReadOutOf(source, object, part);
}

// Whoever reads the value out of the object does with it what is done with the contents of the object.
void FunctionLinker::beReadOutOf(uint32_t source, uint32_t object, Part part)
{
    const LinkedFate& ofObject = fate(object, part);
    if (ofObject.contents.contains(ValueFate::IsIncomplete)) {
        becomeAnything(source, part);
        return;
    }
    LinkedFate readOut = LinkedFate::nothing();
    readOut.itself = ofObject.contents;
    readOut.contents = ofObject.contents;
    if (ofObject.contents.contains(ValueFate::IsReturned))
        readOut.results = UINT32_MAX;
    if (ofObject.contents.containsAny(fatesThatMayRunCodeOfTheObject)) {
        readOut.keysRead = KeysOfMembers::any();
        readOut.keysWritten = KeysOfMembers::any();
    }
    unite(source, part, readOut);
}

OptionSet<ValueFate> FunctionLinker::fatesOfResultOf(ValueID call, Part part)
{
    uint32_t answered = findSubjectOf(call, SubjectKind::CallResult);
    return answered == noSubject ? OptionSet<ValueFate>() : withoutMasks(fate(answered, part));
}

uint32_t FunctionLinker::findSubjectOf(ValueID value, SubjectKind kind) const
{
    auto found = std::ranges::lower_bound(m_subjectsOfValues, std::pair { value.index(), 0u });
    for (; found != m_subjectsOfValues.end() && found->first == value.index(); ++found) {
        if (subject(found->second).kind == kind)
            return found->second;
    }
    return noSubject;
}

uint32_t FunctionLinker::findCallResult(ValueID call, uint32_t result) const
{
    // As read, a call has one result.
    return result ? noSubject : findSubjectOf(call, SubjectKind::CallResult);
}

std::span<const Handing> FunctionLinker::handedAt(ValueID call) const
{
    auto rows = std::ranges::equal_range(m_record.handed, call.index(), { }, [](const Handing& handing) {
        return handing.call.index();
    });
    return m_record.handed.span().subspan(rows.begin() - m_record.handed.begin(), rows.size());
}

std::optional<uint32_t> FunctionLinker::parameterCalledBy(ValueID call, const Value& operation) const
{
    for (const Handing& other : handedAt(call)) {
        if (!isCalledBy(operation, other.operandIndex))
            continue;
        if (subject(other.source).kind == SubjectKind::Parameter)
            return subject(other.source).index;
    }
    return std::nullopt;
}

void FunctionLinker::addRow(HandedToCalledParameter&& row)
{
    for (HandedToCalledParameter& known : m_rows) {
        if (!isSameCall(known, row))
            continue;
        OptionSet<ValueFate> resultOfCall = known.resultOfCall | row.resultOfCall;
        m_hasChanged |= known.resultOfCall != resultOfCall;
        known.resultOfCall = resultOfCall;
        return;
    }
    // In a large cycle of calls every member would come to list the calls of all the others. Who has once given up stays so: else the members of a cycle would take turns for ever.
    if (m_hasTooManyRows)
        return;
    if (m_rows.size() == mostRows) {
        m_hasTooManyRows = true;
        m_hasChanged = true;
        return;
    }
    m_rows.append(WTF::move(row));
    m_hasChanged = true;
}

// The callee is whatever a caller has passed. Who knows the caller knows more.
void FunctionLinker::noteCallOfParameter(const Handing& handing, uint32_t calledParameter)
{
    becomeAnything(handing.source, Part::Whole);
    uint32_t root = m_roots[handing.source];
    std::optional<PlaceAtCall> place = placeAtCall(m_function.at(handing.call), handing.operandIndex);
    if (subject(root).kind != SubjectKind::Parameter || !place) {
        becomeAnything(handing.source, Part::Apart);
        return;
    }
    addRow({ calledParameter, subject(root).index, isReadThroughRoot(handing.source), m_id, handing.call, *place, fatesOfResultOf(handing.call, Part::Whole) });
}

void FunctionLinker::applyFateOfCallee(const Handing& handing, const LinkedFate& ofCallee, Part part, NOESCAPE const Invocable<bool(uint32_t operandIndex, uint32_t bit)> auto& isOperandOfBit)
{
    if (ofCallee.isAnything()) {
        becomeAnything(handing.source, part);
        return;
    }
    LinkedFate applied = ofCallee;
    applied.itself.remove(ValueFate::IsReturned);
    applied.contents.remove(ValueFate::IsReturned);
    // What the callee reads out of the value and returns is what the call answers, too.
    if (ofCallee.contents.contains(ValueFate::IsReturned))
        applied.contents.add(fatesOfResultOf(handing.call, part));
    applied.results = 0;
    applied.storedIntoParameters = 0;
    applied.mayBeStoredDeeper = false;
    applied.storedIntoResults = 0;
    unite(handing.source, part, applied);
    // Nobody here knows what the callee has stored.
    if ((ofCallee.itself | ofCallee.contents).contains(ValueFate::HasMembersWritten))
        m_hasChanged |= m_rootsHoldingUnlistedValues.add(m_roots[handing.source]);

    // What the callee returns is what the call answers, and goes on as that does.
    // The front end lists the result of every call whose result is used, or says of all the operands of the call that it does not know.
    for (uint32_t rest = ofCallee.itself.contains(ValueFate::IsReturned) ? ofCallee.results : 0; rest; rest &= rest - 1) {
        uint32_t answered = findCallResult(handing.call, static_cast<uint32_t>(std::countr_zero(rest)));
        if (answered != noSubject)
            unite(handing.source, part, LinkedFate(fate(answered, part)));
    }

    // What the callee stores into what it returns lies in what the call answers.
    for (uint32_t rest = ofCallee.storedIntoResults; rest; rest &= rest - 1) {
        uint32_t answered = findCallResult(handing.call, static_cast<uint32_t>(std::countr_zero(rest)));
        if (answered != noSubject)
            liveInside(handing.source, answered, part);
    }

    // What the callee stores into another of its parameters lies in what this function has passed there.
    for (uint32_t rest = ofCallee.storedIntoParameters; rest; rest &= rest - 1) {
        uint32_t bit = static_cast<uint32_t>(std::countr_zero(rest));
        bool hasFoundTarget = false;
        for (const Handing& other : handedAt(handing.call)) {
            if (!isOperandOfBit(other.operandIndex, bit))
                continue;
            hasFoundTarget = true;
            noteStore(handing.source, other.source, ofCallee.mayBeStoredDeeper);
        }
        if (!hasFoundTarget)
            addToItself(handing.source, part, ValueFate::IsStoredInUnknownObject);
    }
}

// Somebody below calls the value `calledSubject` and passes what this function has handed on. `row` says so in terms of this function, but for its two parameters.
// The called value is a parameter of this function again, a closure that this function has made, or something else.
void FunctionLinker::composeWithCalledValue(const Handing& handing, uint32_t calledSubject, const HandedToCalledParameter& row, std::span<const Part> parts)
{
    const Program& program = m_link.program;
    const Subject& called = subject(calledSubject);
    uint32_t root = m_roots[handing.source];
    if (called.kind == SubjectKind::Parameter && subject(root).kind == SubjectKind::Parameter) {
        HandedToCalledParameter lifted = row;
        lifted.called = called.index;
        lifted.handed = subject(root).index;
        lifted.isReadThroughHanded |= isReadThroughRoot(handing.source);
        addRow(WTF::move(lifted));
        // For every caller at once, what a caller passes is unknown code.
        if (std::ranges::find(parts, Part::Whole) != parts.end())
            becomeAnything(handing.source, Part::Whole);
        return;
    }
    const Value* made = called.kind == SubjectKind::Allocation ? &m_function.at(ValueID(called.index)) : nullptr;
    if (!made || made->opcode != Opcode::NewClosure || !made->function()) {
        becomeAnything(handing.source, parts);
        return;
    }
    // The closure itself is called: what it has captured comes in as the first entry parameters of its function.
    if (row.place.kind == PlaceAtCall::Kind::Callee) {
        LinkedFate ofCaptures = LinkedFate::nothing();
        for (uint32_t capture = 0; capture < program.at(made->function()).captures.size(); ++capture)
            ofCaptures.contents.add(withoutMasks(m_link.linked.fateOfParameter(made->function(), capture)));
        if (ofCaptures.contents.contains(ValueFate::IsReturned)) {
            ofCaptures.contents.remove(ValueFate::IsReturned);
            ofCaptures.contents.add(row.resultOfCall);
        }
        for (Part part : parts)
            unite(handing.source, part, ofCaptures);
    }
    for (EntryOfPlace entry : entriesAt(program.at(made->function()), row.place)) {
        if (entry.kind == EntryOfPlace::Kind::Unknown) {
            becomeAnything(handing.source, parts);
            return;
        }
        LinkedFate ofClosure = m_link.linked.fateOfParameter(made->function(), entry.parameter);
        // What the closure stores into its other parameters lies in what a call far below has passed.
        if (ofClosure.isAnything() || ofClosure.storedIntoParameters || ofClosure.storedIntoResults) {
            becomeAnything(handing.source, parts);
            return;
        }
        // What the closure returns is what that call answers.
        bool isReturned = ofClosure.itself.contains(ValueFate::IsReturned);
        bool hasReturnedContents = ofClosure.contents.contains(ValueFate::IsReturned);
        ofClosure.itself.remove(ValueFate::IsReturned);
        ofClosure.contents.remove(ValueFate::IsReturned);
        ofClosure.results = 0;
        if (isReturned)
            ofClosure.itself.add(row.resultOfCall);
        if (isReturned || hasReturnedContents)
            ofClosure.contents.add(row.resultOfCall);
        if (ofClosure.itself.contains(ValueFate::IsReturned))
            ofClosure.results = UINT32_MAX;
        if (row.isReadThroughHanded || entry.kind == EntryOfPlace::Kind::ElementOfParameter) {
            LinkedFate ofContents = LinkedFate::nothing();
            ofContents.contents = withoutMasks(ofClosure);
            if (!row.isReadThroughHanded)
                ofContents.itself = ofContents.contents;
            if (ofContents.itself.contains(ValueFate::IsReturned))
                ofContents.results = UINT32_MAX;
            if (ofContents.itself.containsAny(fatesThatMayRunCodeOfTheObject)) {
                ofContents.keysRead = KeysOfMembers::any();
                ofContents.keysWritten = KeysOfMembers::any();
            }
            ofClosure = ofContents;
        }
        for (Part part : parts)
            unite(handing.source, part, ofClosure);
    }
}

// The callee, or one below it, calls what it was passed.
void FunctionLinker::applyRowsOfCallee(const Handing& handing, const Value& call, FunctionID callee, uint32_t entryParameter, bool isElementOfParameter)
{
    // For both parts: where this function knows the closure, its function answers for every caller too.
    for (const HandedToCalledParameter& row : m_link.linked.handedToCalledParameters(callee)) {
        if (row.handed != entryParameter)
            continue;
        // An element of the array that the entry has made for the rest parameter is passed where something read through the parameter is.
        if (isElementOfParameter && !row.isReadThroughHanded)
            continue;
        HandedToCalledParameter here = row;
        here.isReadThroughHanded = row.isReadThroughHanded && !isElementOfParameter;
        // Where the functions below return what the call has answered, it goes on as what this call answers.
        if (here.resultOfCall.contains(ValueFate::IsReturned)) {
            here.resultOfCall.remove(ValueFate::IsReturned);
            here.resultOfCall.add(fatesOfResultOf(handing.call, Part::Whole));
        }
        bool hasFoundCalledValue = false;
        for (const Handing& other : handedAt(handing.call)) {
            if (!isTakenAs(m_link.program.at(callee), call, other.operandIndex, row.called))
                continue;
            hasFoundCalledValue = true;
            composeWithCalledValue(handing, other.source, here, bothParts);
        }
        if (!hasFoundCalledValue)
            becomeAnything(handing.source);
    }
}

// Whether the callee finds the operand in the entry parameter, or in the array that its entry makes for the parameter.
bool FunctionLinker::isTakenAs(const Function& callee, const Value& call, uint32_t operandIndex, uint32_t entryParameter) const
{
    std::optional<PlaceAtCall> place = placeAtCall(call, operandIndex);
    if (!place)
        return entryParameterOf(callee, call, operandIndex) == entryParameter;
    return std::ranges::any_of(entriesAt(callee, *place), [&](EntryOfPlace entry) {
        return entry.kind != EntryOfPlace::Kind::Unknown && entry.parameter == entryParameter;
    });
}

// What becomes of a value that lies in an array of which this becomes.
static LinkedFate asElementOf(const LinkedFate& ofArray)
{
    if (ofArray.isAnything())
        return ofArray;
    LinkedFate ofElement = LinkedFate::nothing();
    ofElement.itself = (ofArray.itself & fatesThatKeep) | ofArray.contents;
    ofElement.contents = ofArray.contents;
    if (ofArray.storedIntoParameters || ofArray.storedIntoResults || ofArray.itself.contains(ValueFate::IsReturned)) {
        ofElement.itself.remove(ValueFate::IsReturned);
        ofElement.itself.add(ValueFate::IsStoredInUnknownObject);
    }
    if (ofArray.contents.contains(ValueFate::IsReturned)) {
        ofElement.itself.add(ValueFate::IsReturned);
        ofElement.results = UINT32_MAX;
    }
    if (ofElement.itself.containsAny(fatesThatMayRunCodeOfTheObject)) {
        ofElement.keysRead = KeysOfMembers::any();
        ofElement.keysWritten = KeysOfMembers::any();
    }
    return ofElement;
}

void FunctionLinker::applyCallee(const Handing& handing, const Value& call, FunctionID callee)
{
    const Function& function = m_link.program.at(callee);
    auto isOperandOfBit = [&](uint32_t operandIndex, uint32_t bit) {
        return isTakenAs(function, call, operandIndex, bit);
    };
    auto apply = [&](uint32_t entryParameter, bool isElementOfParameter) {
        const LinkedFate& apart = m_link.linked.fateApartFromCalledParameters(callee, entryParameter);
        for (Part part : bothParts)
            applyFateOfCallee(handing, isElementOfParameter ? asElementOf(apart) : apart, part, isOperandOfBit);
        applyRowsOfCallee(handing, call, callee, entryParameter, isElementOfParameter);
    };

    std::optional<PlaceAtCall> place = placeAtCall(call, handing.operandIndex);
    if (!place) {
        if (std::optional<uint32_t> entryParameter = entryParameterOf(function, call, handing.operandIndex))
            apply(*entryParameter, false);
        else
            becomeAnything(handing.source);
        return;
    }
    // No entry: the callee cannot see the value.
    for (EntryOfPlace entry : entriesAt(function, *place)) {
        if (entry.kind == EntryOfPlace::Kind::Unknown)
            becomeAnything(handing.source);
        else
            apply(entry.parameter, entry.kind == EntryOfPlace::Kind::ElementOfParameter);
    }
}

void FunctionLinker::applyBuiltin(const Handing& handing, const Value& call, BuiltinID builtin)
{
    auto isOperandOfBit = [](uint32_t operandIndex, uint32_t bit) {
        return operandIndex == bit;
    };
    // The table of built-ins says the same at every turn of a visit.
    const AnswerOfBuiltin& answer = answerOf(handing, call, builtin);
    // Where the types of the operands list the functions that native code calls, those have answered. Else the rows are followed for every caller too.
    bool hasFollowedCallbacks = !answer.withCallbacks.isAnything();
    applyFateOfCallee(handing, hasFollowedCallbacks ? answer.withCallbacks : answer.withoutCallbacks, Part::Whole, isOperandOfBit);
    applyFateOfCallee(handing, answer.withoutCallbacks, Part::Apart, isOperandOfBit);
    std::span<const Part> partsOfRows = hasFollowedCallbacks ? std::span<const Part>(bothParts).subspan(1) : std::span<const Part>(bothParts);
    for (const CallbackOfBuiltin& callback : answer.callbacks) {
        HandedToCalledParameter row { 0, 0, callback.isReadThroughHanded, m_id, handing.call, callback.place, callback.resultOfCall };
        // What native code returns of it is what the call answers.
        if (row.resultOfCall.contains(ValueFate::IsReturned)) {
            row.resultOfCall.remove(ValueFate::IsReturned);
            row.resultOfCall.add(fatesOfResultOf(handing.call, Part::Whole));
        }
        bool hasFoundCalledValue = false;
        for (const Handing& other : handedAt(handing.call)) {
            if (other.operandIndex != callback.called)
                continue;
            hasFoundCalledValue = true;
            composeWithCalledValue(handing, other.source, row, partsOfRows);
        }
        if (!hasFoundCalledValue)
            becomeAnything(handing.source, partsOfRows);
    }
}

const FunctionLinker::AnswerOfBuiltin& FunctionLinker::answerOf(const Handing& handing, const Value& call, BuiltinID builtin)
{
    uint32_t row = static_cast<uint32_t>(&handing - m_record.handed.span().data());
    for (const AnswerOfBuiltin& known : m_answersOfBuiltins) {
        if (known.row == row && known.builtin == builtin)
            return known;
    }
    AnswerOfBuiltin answer;
    answer.row = row;
    answer.builtin = builtin;
    answer.withCallbacks = fateOfOperandOfBuiltin(m_link, m_function, builtin, call, handing.operandIndex, Callbacks::AreFollowed);
    answer.withoutCallbacks = fateOfOperandOfBuiltin(m_link, m_function, builtin, call, handing.operandIndex, Callbacks::AreLeftOut);
    forEachCallbackOfBuiltin(m_link.program, &m_function, builtin, call, [&](const CallbackOfBuiltin& callback) {
        if (callback.handed == handing.operandIndex)
            answer.callbacks.append(callback);
    });
    m_answersOfBuiltins.append(WTF::move(answer));
    return m_answersOfBuiltins.last();
}

// The value is called. The function that it is a closure of takes it as the callee, and what it has captured as the first of its entry parameters.
void FunctionLinker::followCallOf(const Handing& handing, const Value& call, const CalleeList& callees)
{
    // A built-in has captured nothing and never takes its callee: nothing becomes of it.
    if (!callees.isComplete) {
        // Nobody knows the function. It may keep what it has captured, and itself: it knows its own name.
        becomeAnything(handing.source, Part::Whole);
        std::optional<PlaceAtCall> place = placeAtCall(call, handing.operandIndex);
        if (subject(handing.source).kind == SubjectKind::Parameter && place)
            addRow({ subject(handing.source).index, subject(handing.source).index, false, m_id, handing.call, *place, fatesOfResultOf(handing.call, Part::Whole) });
        else
            becomeAnything(handing.source, Part::Apart);
        return;
    }
    for (FunctionID callee : callees.functions) {
        const Function& function = m_link.program.at(callee);
        // What the function does with a capture that it calls is not followed here.
        auto fateIn = [&](uint32_t entryParameter, Part) -> const LinkedFate& {
            return m_link.linked.fateOfParameter(callee, entryParameter);
        };
        for (uint32_t capture = 0; capture < function.captures.size(); ++capture) {
            for (Part part : bothParts) {
                LinkedFate ofContents = LinkedFate::nothing();
                ofContents.contents = withoutMasks(fateIn(capture, part));
                // What the function returns of them is what the call answers.
                if (ofContents.contents.contains(ValueFate::IsReturned)) {
                    ofContents.contents.remove(ValueFate::IsReturned);
                    ofContents.contents.add(fatesOfResultOf(handing.call, part));
                }
                unite(handing.source, part, ofContents);
            }
        }
        auto isOperandOfBit = [&](uint32_t operandIndex, uint32_t bit) {
            return isTakenAs(function, call, operandIndex, bit);
        };
        for (uint32_t entryParameter : entryParametersOfCalledValue(function, call)) {
            for (Part part : bothParts)
                applyFateOfCallee(handing, m_link.linked.fateApartFromCalledParameters(callee, entryParameter), part, isOperandOfBit);
            applyRowsOfCallee(handing, call, callee, entryParameter, false);
        }
    }
}

// The function makes a closure, and the value is what the closure captures: the operands of the operation are the first entry parameters of the closure's function.
// The value lives as long as the closure, and whenever somebody calls the closure its function does with the value what it does with that parameter.
void FunctionLinker::followCapture(const Handing& handing, const Value& closure)
{
    uint32_t made = findSubjectOf(handing.call, SubjectKind::Allocation);
    if (made == noSubject || !closure.function()) {
        becomeAnything(handing.source);
        return;
    }
    m_hasChanged |= m_closuresCapturedBy[handing.source].add(made);
    // A function that flow never reaches has lost its body: it never runs, so nothing becomes of what it has captured but that it lives as long as the closure.
    LinkedFate ofCapture = m_link.program.at(closure.function()).hasBody() ? m_link.linked.fateOfParameter(closure.function(), handing.operandIndex) : LinkedFate::nothing();
    if (ofCapture.isAnything()) {
        becomeAnything(handing.source);
        return;
    }
    // What the function stores into another of its captures lies in what the closure has captured there.
    uint32_t numberOfCaptures = m_link.program.at(closure.function()).captures.size();
    uint32_t storedElsewhere = ofCapture.storedIntoParameters;
    for (uint32_t rest = ofCapture.storedIntoParameters; rest; rest &= rest - 1) {
        uint32_t capture = static_cast<uint32_t>(std::countr_zero(rest));
        if (capture >= numberOfCaptures)
            continue;
        for (const Handing& other : handedAt(handing.call)) {
            if (other.operandIndex != capture)
                continue;
            storedElsewhere &= ~(1u << capture);
            noteStore(handing.source, other.source, ofCapture.mayBeStoredDeeper);
        }
    }
    // What the function returns goes to whoever calls the closure, and what it stores into its other parameters lies in what that caller has passed.
    if (ofCapture.itself.contains(ValueFate::IsReturned) || storedElsewhere || ofCapture.storedIntoResults)
        ofCapture.itself.add(ValueFate::IsStoredInUnknownObject);
    if ((ofCapture.itself | ofCapture.contents).contains(ValueFate::HasMembersWritten))
        m_hasChanged |= m_rootsHoldingUnlistedValues.add(m_roots[handing.source]);
    if (ofCapture.contents.contains(ValueFate::IsReturned))
        ofCapture.contents.add(ValueFate::IsStoredInUnknownObject);
    ofCapture.itself.remove(ValueFate::IsReturned);
    ofCapture.contents.remove(ValueFate::IsReturned);
    ofCapture.results = 0;
    ofCapture.storedIntoParameters = 0;
    ofCapture.mayBeStoredDeeper = false;
    ofCapture.storedIntoResults = 0;
    for (Part part : bothParts)
        unite(handing.source, part, ofCapture);
}

// The rows of one call stand together, and every row is followed again and again until nothing changes.
const CalleeList& FunctionLinker::calleesOf(ValueID id, const Value& call)
{
    return *m_calleesOfCalls.ensure(id.index(), [&] {
        return makeUniqueWithoutFastMallocCheck<CalleeList>(calleesFollowedAt(m_link.program, call));
    }).iterator->value;
}

void FunctionLinker::followHanding(const Handing& handing)
{
    // Nothing can be added.
    if (m_states[handing.source].whole.isAnything() && m_states[handing.source].apart.isAnything())
        return;
    const Value& call = m_function.at(handing.call);
    if (call.opcode == Opcode::NewClosure) {
        followCapture(handing, call);
        return;
    }
    const CalleeList& callees = calleesOf(handing.call, call);
    if (isCalledBy(call, handing.operandIndex)) {
        followCallOf(handing, call, callees);
        return;
    }
    if (!callees.isComplete) {
        if (std::optional<uint32_t> calledParameter = parameterCalledBy(handing.call, call)) {
            noteCallOfParameter(handing, *calledParameter);
            return;
        }
        becomeAnything(handing.source);
        return;
    }
    for (FunctionID callee : callees.functions)
        applyCallee(handing, call, callee);
    for (BuiltinID builtin : callees.builtins)
        applyBuiltin(handing, call, builtin);
}

// `const b = new Box(); b.x = v`: a store may find a setter, a read a getter, which get hold of the object and of what is stored. Of a parameter the caller asks, who knows its object.
// Of an object that the function has made itself nobody else will ever ask.
void FunctionLinker::askOwnObjects()
{
    for (uint32_t index = 0; index < m_record.subjects.size(); ++index) {
        SubjectKind kind = subject(index).kind;
        if (kind != SubjectKind::Allocation && kind != SubjectKind::CallResult)
            continue;
        // The answer follows from what is known of the subject alone.
        if (m_states[index].versionWhenAsked == m_states[index].version)
            continue;
        m_states[index].versionWhenAsked = m_states[index].version;
        for (Part part : bothParts) {
            const LinkedFate& known = fate(index, part);
            if (known.isAnything() || !known.itself.containsAny(fatesThatMayRunCodeOfTheObject))
                continue;
            if (!m_proofs)
                m_proofs.emplace(m_function);
            if (runsNoCodeOnObjectMadeBy(m_link.program, m_link.flow, m_function, *m_proofs, ValueID(subject(index).index), known))
                continue;
            if (kind == SubjectKind::Allocation || isFreshObject(index)) {
                becomeAnything(index, part);
                continue;
            }
            // `same(p).x`: what the call answers may be a parameter, of which the caller asks: the flags stay open. What lies in the answer is within reach of the code that runs.
            if (m_resultsWhoseCodeMayRun[static_cast<unsigned>(part)].add(index))
                didChange(index);
        }
    }
}

void FunctionLinker::addRowsOfLastVisit(std::span<const HandedToCalledParameter> rows)
{
    for (const HandedToCalledParameter& row : rows)
        addRow(HandedToCalledParameter(row));
}

void FunctionLinker::run()
{
    findRoots();
    start();
    // What a call has answered holds what its callees have put into it.
    for (uint32_t index = 0; index < m_record.subjects.size(); ++index) {
        if (subject(index).kind == SubjectKind::CallResult)
            m_rootsHoldingUnlistedValues.add(index);
    }
    for (const Store& store : m_record.stores) {
        // What is stored may be no subject. The row is there for its key.
        if (store.source != noSubject)
            noteStore(store.source, store.target, false);
        else
            m_rootsHoldingUnlistedValues.add(m_roots[store.target]);
    }
    do {
        m_hasChanged = false;
        for (uint32_t row = 0; row < m_record.handed.size(); ++row) {
            // Of all that changes in a visit a row reads only what is known of the subject that its call answers. While that stands, the row adds what it has added before.
            StateOfHanding& state = m_rowStates[row];
            uint32_t version = state.answer == noSubject ? 0 : m_states[state.answer].version;
            if (state.versionWhenFollowed == version)
                continue;
            state.versionWhenFollowed = version;
            followHanding(m_record.handed[row]);
        }
        closeStores();
        followStores();
        followReads();
        askOwnObjects();
    } while (m_hasChanged);
    if (m_hasTooManyRows) {
        m_rows.clear();
        for (StateOfSubject& state : m_states)
            state.apart = state.whole;
    }
}

LinkedFate FunctionLinker::fateOfParameter(uint32_t entryParameter, bool apartFromCalledParameters) const
{
    for (uint32_t index = 0; index < m_record.subjects.size(); ++index) {
        if (subject(index).kind == SubjectKind::Parameter && subject(index).index == entryParameter)
            return apartFromCalledParameters ? m_states[index].apart : m_states[index].whole;
    }
    return LinkedFate::anything();
}

Vector<LinkedFate> FunctionLinker::fatesOfSubjects() const
{
    return m_states.map([](const StateOfSubject& state) {
        return state.whole;
    });
}

} // anonymous namespace

std::unique_ptr<RelationalSummaries> linkRelationalSummaries(const Program& program, const FlowResults& flow, BuildContext& build, LinkedFunctions linkedFunctions)
{
    const ParallelLoops& loops = build.parallelLoops();
    unsigned numberOfFunctions = program.numberOfFunctionsAsRead();
    auto summaries = makeUnique<RelationalSummaries>();

    Vector<std::optional<Record>> records(numberOfFunctions);
    // A function comes behind the functions that it hands something to.
    Vector<Vector<uint32_t, 4>> callees(numberOfFunctions);
    auto listCallees = [&](unsigned index) {
        Vector<uint32_t, 4> found;
        ValueID last;
        for (const Handing& handing : records[index]->handed) {
            if (handing.call == last)
                continue;
            last = handing.call;
            const Value& call = program.functions[index].at(handing.call);
            if (call.opcode == Opcode::NewClosure) {
                if (call.function() && call.function().index() < numberOfFunctions)
                    found.append(call.function().index());
                continue;
            }
            CalleeList calleesOfThisCall = calleesFollowedAt(program, call);
            for (FunctionID callee : calleesOfThisCall.functions) {
                if (callee.index() < numberOfFunctions)
                    found.append(callee.index());
            }
            // Native code calls what it is handed: the type of an operand lists the functions.
            if (calleesOfThisCall.builtins.isEmpty())
                continue;
            for (uint32_t operandIndex = 0; operandIndex < call.numberOfOperands(); ++operandIndex) {
                if (!call.operand(operandIndex))
                    continue;
                for (FunctionID callback : calleesOf(program, program.typeOf(program.functions[index], call.operand(operandIndex))).functions) {
                    if (callback.index() < numberOfFunctions)
                        found.append(callback.index());
                }
            }
        }
        for (const Subject& subject : records[index]->subjects) {
            // What a call answers is what its callees return.
            if (subject.kind == SubjectKind::CallResult) {
                for (FunctionID callee : calleesFollowedAt(program, program.functions[index].at(ValueID(subject.index))).functions) {
                    if (callee.index() < numberOfFunctions)
                        found.append(callee.index());
                }
            }
            // A closure that the function makes and hands to a callee which calls it.
            if (subject.kind != SubjectKind::Allocation)
                continue;
            const Value& made = program.functions[index].at(ValueID(subject.index));
            if (made.opcode == Opcode::NewClosure && made.function() && made.function().index() < numberOfFunctions)
                found.append(made.function().index());
        }
        std::ranges::sort(found);
        found.shrink(std::ranges::unique(found).begin() - found.begin());
        return found;
    };

    Vector<uint32_t> functionsToRead = findIndicesInParallel(loops, numberOfFunctions, [&](unsigned index) {
        return linkedFunctions == LinkedFunctions::All ? program.functions[index].hasBody() : mayBeTypedByOrigins(program.functions[index]);
    });
    BitVector isRead(numberOfFunctions);
    for (uint32_t index : functionsToRead)
        isRead.quickSet(index);
    Vector<uint32_t> functionsWithRecord;
    while (!functionsToRead.isEmpty()) {
        forEachIndexInParallel(loops, functionsToRead.size(), [&](unsigned position) {
            uint32_t index = functionsToRead[position];
            if (!program.functions[index].hasBody())
                return;
            records[index] = recordOf(program.functions[index]);
            if (records[index])
                callees[index] = listCallees(index);
        });
        Vector<uint32_t> functionsToReadNext;
        for (uint32_t index : functionsToRead) {
            if (!records[index])
                continue;
            functionsWithRecord.append(index);
            for (uint32_t callee : callees[index]) {
                if (!isRead.quickSet(callee))
                    functionsToReadNext.append(callee);
            }
        }
        functionsToRead = WTF::move(functionsToReadNext);
    }
    if (functionsWithRecord.isEmpty())
        return summaries;
    forEachIndexInParallel(loops, functionsWithRecord.size(), [&](unsigned position) {
        callees[functionsWithRecord[position]].removeAllMatching([&](uint32_t callee) {
            return !records[callee];
        });
    });
    StronglyConnectedComponents components = computeStronglyConnectedComponents(numberOfFunctions, [&](unsigned index) -> const Vector<uint32_t, 4>& {
        return callees[index];
    });

    // The components are numbered from the callees up.
    Vector<uint32_t> levelOfComponent;
    levelOfComponent.fill(0, components.numberOfComponents);
    uint32_t numberOfLevels = 0;
    for (uint32_t component = 0; component < components.numberOfComponents; ++component) {
        for (uint32_t member : components.membersOf(component)) {
            for (uint32_t callee : callees[member]) {
                uint32_t below = components.componentOf[callee];
                RELEASE_ASSERT(below <= component);
                if (below != component)
                    levelOfComponent[component] = std::max(levelOfComponent[component], levelOfComponent[below] + 1);
            }
        }
        numberOfLevels = std::max(numberOfLevels, levelOfComponent[component] + 1);
    }
    Vector<Vector<uint32_t>> componentsOfLevel(numberOfLevels);
    for (uint32_t component = 0; component < components.numberOfComponents; ++component)
        componentsOfLevel[levelOfComponent[component]].append(component);

    summaries->m_functions.grow(numberOfFunctions);
    RelationalSummaries::Order order;
    order.componentOf = components.componentOf;
    order.levelOf = Vector<uint32_t>(numberOfFunctions, [&](size_t index) {
        return levelOfComponent[components.componentOf[index]];
    });
    summaries->m_orderWhileLinking = &order;
    RelationalLink link(program, flow, *summaries);
    auto numberOfParametersOf = [&](uint32_t index) {
        return program.functions[index].at(entryBlock).parameters.size();
    };
    // Reads the table and does not write it: the members of a cycle are visited side by side.
    auto visit = [&](uint32_t index, bool& hasChanged) {
        const RelationalSummaries::OfFunction& summary = summaries->m_functions[index];
        RelationalSummaries::setAsker(order.levelOf[index], order.componentOf[index]);
        // What a visit finds is added to what the last visit has found: so the answers of a cycle only grow, and the rounds end.
        FunctionLinker linker(link, FunctionID(index), *records[index], summary.hasTooManyRows);
        linker.addRowsOfLastVisit(summary.handedToCalledParameters.span());
        linker.run();
        RelationalSummaries::OfFunction found;
        found.numberOfEntryParameters = numberOfParametersOf(index);
        found.parameters = Vector<LinkedFate>(numberOfParametersOf(index), [&](size_t parameter) {
            LinkedFate fate = linker.fateOfParameter(static_cast<uint32_t>(parameter), false);
            fate.unite(summary.parameters[parameter]);
            return fate;
        });
        found.parametersApartFromCalledParameters = Vector<LinkedFate>(numberOfParametersOf(index), [&](size_t parameter) {
            LinkedFate fate = linker.fateOfParameter(static_cast<uint32_t>(parameter), true);
            fate.unite(summary.parametersApartFromCalledParameters[parameter]);
            return fate;
        });
        found.handedToCalledParameters = linker.takeRows();
        found.hasTooManyRows = linker.hasTooManyRows();
        // The origins go on from what the last visit has found, which the table still holds for a function that calls itself.
        found.results = summary.results;
        Vector<LinkedFate> fatesOfSubjects = linker.fatesOfSubjects();
        hasChanged = linkResultOrigins(link, FunctionID(index), fatesOfSubjects.span(), found.results);
        hasChanged |= found.parameters != summary.parameters || found.parametersApartFromCalledParameters != summary.parametersApartFromCalledParameters
            || found.handedToCalledParameters != summary.handedToCalledParameters || found.hasTooManyRows != summary.hasTooManyRows;
        return found;
    };
    // Round by round: every member that calls one whose answer has changed is visited again, all of a round side by side on what the round before has left.
    // So the answer does not hang on the number of threads.
    auto visitCycle = [&](std::span<const uint32_t> members) {
        uint32_t component = components.componentOf[members[0]];
        HashMap<uint32_t, Vector<uint32_t>, IntHash<uint32_t>, WTF::UnsignedWithZeroKeyHashTraits<uint32_t>> callersInCycle;
        for (uint32_t member : members) {
            for (uint32_t callee : callees[member]) {
                if (components.componentOf[callee] == component)
                    callersInCycle.ensure(callee, [] { return Vector<uint32_t>(); }).iterator->value.append(member);
            }
        }
        Vector<uint32_t> toVisit;
        for (uint32_t member : members) {
            if (records[member])
                toVisit.append(member);
        }
        while (!toVisit.isEmpty()) {
            Vector<RelationalSummaries::OfFunction> found(toVisit.size());
            Vector<uint8_t> hasChanged(toVisit.size());
            forEachIndexInParallel(loops, toVisit.size(), [&](unsigned position) {
                bool changed = false;
                found[position] = visit(toVisit[position], changed);
                hasChanged[position] = changed;
            });
            Vector<uint32_t> next;
            for (unsigned position = 0; position < toVisit.size(); ++position) {
                if (!hasChanged[position])
                    continue;
                summaries->m_functions[toVisit[position]] = WTF::move(found[position]);
                if (auto callers = callersInCycle.find(toVisit[position]); callers != callersInCycle.end())
                    next.appendVector(callers->value);
            }
            std::ranges::sort(next);
            next.shrink(std::ranges::unique(next).begin() - next.begin());
            toVisit = WTF::move(next);
        }
    };
    for (const Vector<uint32_t>& level : componentsOfLevel) {
        forEachIndexInParallel(loops, level.size(), [&](unsigned position) {
            std::span<const uint32_t> members = components.membersOf(level[position]);
            // Inside a cycle of calls every member starts with "nothing becomes of it", and what is known only grows.
            for (uint32_t member : members) {
                if (!records[member])
                    continue;
                summaries->m_functions[member].parameters.fill(LinkedFate::nothing(), numberOfParametersOf(member));
                summaries->m_functions[member].parametersApartFromCalledParameters.fill(LinkedFate::nothing(), numberOfParametersOf(member));
                summaries->m_functions[member].results.fill(ResultOrigin::nothingYet(), program.functions[member].results.size());
            }
            if (members.size() > 1 || components.isInCycle.get(members[0])) {
                visitCycle(members);
                return;
            }
            if (records[members[0]]) {
                bool hasChanged = false;
                summaries->m_functions[members[0]] = visit(members[0], hasChanged);
            }
        });
    }
    summaries->m_orderWhileLinking = nullptr;
    // The table lives to the end of the build.
    forEachIndexInParallel(loops, numberOfFunctions, [&](unsigned index) {
        RelationalSummaries::OfFunction& summary = summaries->m_functions[index];
        if (summary.parametersApartFromCalledParameters == summary.parameters)
            summary.parametersApartFromCalledParameters = { };
        bool saysSomething = !summary.parametersApartFromCalledParameters.isEmpty() || std::ranges::any_of(summary.parameters, [](const LinkedFate& fate) {
            return !fate.isAnything();
        });
        if (!saysSomething)
            summary.parameters = { };
        keepOriginsThatDependOnTheCall(summary.results);
    });
    if (build.options().logging.logsPhaseTimes) [[unlikely]] {
        unsigned functions = 0, parameters = 0, anything = 0, keptNowhere = 0, keptNowhereWhateverTheObject = 0, betterApart = 0, rows = 0, results = 0, fresh = 0, fullyListed = 0;
        for (unsigned index = 0; index < numberOfFunctions; ++index) {
            if (!records[index])
                continue;
            const RelationalSummaries::OfFunction& summary = summaries->m_functions[index];
            ++functions;
            rows += summary.handedToCalledParameters.size();
            if (summary.parameters.isEmpty()) {
                parameters += numberOfParametersOf(index);
                anything += numberOfParametersOf(index);
            }
            for (unsigned parameter = 0; parameter < summary.parameters.size(); ++parameter) {
                const LinkedFate& fate = summary.parameters[parameter];
                ++parameters;
                anything += fate.isAnything();
                keptNowhere += isKeptNowhere(fate);
                keptNowhereWhateverTheObject += isKeptNowhere(fate) && !fate.itself.containsAny(fatesThatMayRunCodeOfTheObject);
                betterApart += !summary.parametersApartFromCalledParameters.isEmpty() && fate != summary.parametersApartFromCalledParameters[parameter];
            }
            for (const ResultOrigin& origin : summary.results) {
                ++results;
                fresh += origin.isFresh;
                fullyListed += origin.isFullyListed;
            }
        }
        dataLogLn("AOT relational summaries: functions with a record ", functions, " | levels ", numberOfLevels, " | entry parameters ", parameters, " | anything ", anything, " | kept nowhere ", keptNowhere,
            " | .. whatever the object ", keptNowhereWhateverTheObject, " | better apart from called parameters ", betterApart, " | rows ", rows, " | results ", results, " | fully listed ", fullyListed, " | fresh ", fresh);
    }
    return summaries;
}

} } } // namespace JSC::AOT::Typed

#endif // ENABLE(AOT)
