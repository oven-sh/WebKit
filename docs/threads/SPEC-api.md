# SPEC-api.md - FROZEN implementation spec (rev 14)

Workstream:Thread/Lock/Condition/ThreadLocal JS API,Atomics-on-props,test corpus. Branch jarred/threads;design doc THREAD.md. FROZEN:implement as written;ambiguity=spec bug. Rev 14=rev 13+r4 fixes (§2 re-scope/gate split,§3 prep,hist r4);logs:SPEC-api-history.md ("hist",non-normative). Scope:phase 1=GIL'd Thread() (THREAD.md:23);semantics final except "GPO"=GIL-phase-only clauses. Abbrev:CAE=ConcurrentAccessError,DAL=JSLock::DropAllLocks,DWT=DeferredWorkTimer,WLM=WaiterListManager,SVZ=SameValueZero,Dev=Deviation,TM=ThreadManager,TS=ThreadState,PWT=PropertyWaiterTable,QL=m_queueLock,LL=listLock,AT=AsyncTicket,JSL=the JSLock,u/JSL(u/GIL)=under JSL(GIL),uJT()=Options::useJSThreads(),GI=GIL-INDEPENDENT(holds post-GIL),RL=runloop,prop(s)=property(ies),WS=workstream,implr=implementer,JT/=JSTests/threads/,AO=AtomicsObject.cpp,NLS=NativeLockState,NCS=NativeConditionState. Bare numbers=spec sections.

## 1. Grounding index (in-tree;paths under Source/JavaScriptCore/,wtf/=Source/WTF/wtf/)

G4 OptionsList.h:680. G5 wtf/text/AtomStringImpl.cpp:40-64. G6 AtomStringImpl.cpp:68-71,JSLock.cpp:124. G7 JSLock.h:40-73. G8 VMManager.h:73-316. G10 WLM.cpp:120-145,AO:443-478. G11 AO:459-462. G12 JSGlobalObject.cpp:1623-1628. G13 JSDestructibleObject.h:34,VM.h:485. G15 bench/harness.js:1-16,bench-gate.sh:13-72. G16 run-jsc-stress-tests:1029. G19 JSObject.h:1459,1471,JSObject.cpp:661-669. G20 StructureTransitionTable.h:44-68. G21 JSObject.h:820-848,Structure.h:315. G22 wtf/Lock.h:75-129,ParkingLot.h:63-112. G23 VMTraps.h:149-156. G24 JSLock.cpp:137. G25 Repatch.cpp:348-354,Structure.h:884-904 (more:hist). G26 wtf/RunLoop.h:344. G27 WLM.cpp:135-142. G28 VM.h:439,JSRunLoopTimer.cpp:45,DWT.cpp:181. G29 JSGlobalObject.cpp:2416-2433,3345-3379 (more:hist). G30 AO:441-477,VM.cpp:1631-1633. G31 PropertySlot.h:122-155. G32 Structure.h:782,907. G33 SPEC-objectmodel.md:5,388. G34 jsc.cpp:4281,4439,SimpleTypedArrayController.cpp:60-63. G35 SPEC-jit.md:211,238,256 (P5/R5/CS3). G36 SPEC-vmstate.md:66-68,517-530,551-568,647-655 (R2,6.5.1,6.7,M_opts2/M4). Cross-spec cites:anchors govern on drift.

## 2. Deviations from THREAD.md

1. THREAD.md:7 - two stop-the-world clients,not one (G8).
2. Atom-table "flip">flip (per-WTF-thr+JSLock swap,G5/G6). Implement phase keeps per-VM swap;at INT M_opts2 (G36):uJT() implies its 3 flags;M4 KEEPS the swap,only change=6.4.4 install/restore (vmstate 6.4.4);corpus passes both regimes.
3. Raw-slot-address wait keying (G10) unsound for props (slots move);prop waits key (JSCell*,UniquedStringImpl*) (5.6).
4. No Atomics.wake here;extend notify only.
5. No browser main-thr rule;blocking gate=G11 (per-VM u/GIL,I18):join(),lock.hold(),cond.wait().
6. Butterfly-tagging claims (THREAD.md:9) unverified;nothing here depends on them.
7. asyncHold(no-fn)/asyncWait release=explicit release fn;with-fn arity=implicit E (4.2/4.3,5.5a).
8. Thread.restrict narrowed (G19/G20;hist). Enforced:get,set,has,delete,defineProperty,ownKeys,setPrototypeOf,isExtensible,preventExtensions,+indexed set/delete/define (ByIndex). NOT enforced (doc'd):getPrototypeOf,call/construct,indexed GET (sound:get_by_val honors SlowPut shape;hist). Excluded receivers (=>TypeError "cannot restrict this object"):global obj,global proxy,Proxy,environment/scope objs,species-protected builtin proto/ctor pairs:Array,Promise,RegExp,ArrayBuffer+SharedArrayBuffer (both modes),each %TypedArray% view+super pair (G29). Detect:ptr-compare o vs o's own JSGlobalObject slots;never force lazy slots (G25/G29;hist).
9. Phase-1 GIL preemption cooperative-only (G23/G24;yields=5.2 blocking primitives only);later:NeedStopTheWorld+VMManager.
10. TID recycling:phase 1 NONE;GC-time rebias=CHARTERED-OWNED (OM 8c r12/Task 13+task 15 here):a shared-GC stop restamps dead-TID butterfly tags+structure TIDs to 0,TM then reissues via m_freeTIDs;until landed,exhaustion=>RangeError at spawn (5.1,I17).
11. Thread.restrict also excludes hijacksIndexingHeader() structures (JSObject.cpp:1990-1991;5.7.1 needs ArrayStorage)=>TypeError as Dev 8.
12. 5.6/4.5 post-GIL re-freeze (atomic slot CAS/RMW added to OM §9.5 then)=UNOWNED chartered WS;INTEGRATE records sign-off (OM 8g).

Review logs:hist;no useConcurrentJS alias (9.2-1);R4 FP (hist).

Composed deliverable (r14,binds all five specs):the fan-out lands ONLY the GIL'd Thread() semantics milestone+flag-gated infrastructure;THREAD.md's N-mutator perf contract is EXPLICITLY phase-2. GIL removal+near-baseline N-mutator perf GATED on charters,ALL chartered (owner+frozen interface+budget) by the orchestrator BEFORE GIL removal,INTEGRATE records sign-off: heap Dev 7 (incl. per-THREAD TLC addressing);heap §3.8 per-thread-client model;vmstate Dev 10 Phase B incl. thread-granular STW (VMM counts entered THREADS per VM;jit R1.c re-frozen there) — HARD precondition,jit Task-13 INTEGRATION-GATE validates N-separate-VMs only;OM Tasks 13-14 (14 decided PRE-INT on the GIL-stub construction bench);jit §4.3 revival;Dev 12/OM 8g (atomic slot CAS/RMW+PWT re-home+4.5-1a lift). Until OM Task 14,concurrent prop adds on shared shapes=cell-locked+structure-table-locked (OM 8h/L6/I37);flag-on 1-thread budget=jit Task-13 gate ({useJSThreads=1,useSharedGCHeap=0} <=5%;{1,1} recorded — heap Dev-7 split).

## 3. Configuration surface (Options)

OptionsList.h not implr-editable;9.2-1 is orchestrator-PRE-APPLIED before fan-out (jit §10 prep;this text canonical);absent=>STOP+escalate,NO local patch. Other 9.2 hunks:heap-§14-style private overlay,never committed.

| Option | Type | Default | Meaning |
|---|---|---|---|
| useJSThreads | Bool | false | Master switch (I1). |
| maxJSThreads | Unsigned | 32766 | Max live Threads;exceed=>RangeError (TID space:5.1/7). |
| jsThreadGILTimeSliceMs | Unsigned | 0 | Reserved,inert phase 1 (Dev 9). |
| jsThreadStackSizeKB | Unsigned | 0 | Spawned stack size;0=WTF::Thread default. |

useJSThreads needs SAB semantics,not useSharedArrayBuffer flag (G4). Flag off=>all obj-model gating off. 3-flag implication at INT:Dev 2. Dedupe/no-alias:9.2-1.

## 4. Public JS API (exact)

Constructors=global props,DontEnum;installed eagerly in JSGlobalObject::init() u/uJT() via putDirectWithoutTransition (G12;9.2-2 sole mechanism;flag off=>no own prop,I1). No-new=>TypeError. Instances=JSDestructibleObject subclasses (G13),ordinary protos,Symbol.toStringTag each.

### 4.1 Thread

- new Thread(fn,...args):fn callable else TypeError ("Thread constructor requires a callable argument");spawns immediately;runs fn(...args),this===undefined;returned JSThread===Thread.current inside;fn/args rooted (5.10).
- thr.join():blocks;returns fn's value/rethrows its exc;all joins agree (any thr/count/time);self-join=>Error ("Thread cannot join itself");blocking disallowed (G11)=>TypeError;GIL released while blocked.
- thr.asyncJoin():Promise (result/exc);never blocks;5.5 tkt;repeat calls:distinct promises,same settle.
- thr.id:engine TID (main 0). Thread.current:caller's JSThread;main/embedder thrs create lazily on first access,stable.
- Thread.restrict(o):non-obj or Dev-8/11 excluded receiver=>TypeError;marks o restricted to caller,returns o;idempotent from owner;re-restrict from another thr=>CAE;every Dev-8 enforced op from another thr=>CAE;side effects 5.7.1 (conversions,perf only).
- CAE:global ctor,Error subclass,name "ConcurrentAccessError".

Lifecycle:Running->Finished(result)|Failed(exc);no detach/cancel.

### 4.2 Lock

- new Lock():non-recursive.
- lock.hold(fn):fn callable else TypeError;caller already holds (5.3)=>Error ("Lock is not recursive");tryLock first (uncontended never blocks,always allowed);contended:G11 disallows=>TypeError else drop GIL+block;run fn(),release finally-equivalent (exception 4.3(a):hold consumed,returns w/o lock;5.3 guard),return/rethrow.
- lock.asyncHold(fn?):with fn:Promise;once granted (5.5a) fn() runs on RL turn holding lock;settles w/ fn's result/exc after implicit post-fn release (5.5a E). Without fn:Promise of release fn to call exactly once:twice=>Error;never=>acquirers stall (doc'd). Caller sync-holds=>Error ("Lock is not recursive");async-held is NOT recur (callers queue).
- lock.locked:m_lock.isLocked()||m_asyncHeld;tests only;racy.
- Sync-vs-async order unspecified (barging allowed);async tkts FIFO.

### 4.3 Condition

- cond.wait(lock):lock must be Lock held by caller (5.3) else TypeError. Atomically (5.4):enqueue,release lock,block (GIL released);on wakeup reacq lock,return undefined. Spurious wakeups allowed;predicate loops mandatory;G11-gated.
- cond.asyncWait(lock):lock must be (a) sync-held by caller (5.3 m_holder) or (b) async-held (live m_asyncHolder tkt);else TypeError. BOTH consume the hold ((b) unvalidated;outstanding release then throws 4.2 Error). Releases lock now ((a) clear m_holder+unlock,hold epilogue skips release (5.3);(b) 5.5a async-release),pumps R. Promise:on notify tkt joins async-acquirer queue;granted=>resolves on RL turn w/ fresh release (no-fn contract).
- cond.notify()/notifyAll():wake one/all (sync+async uniformly,FIFO across kinds);returns count woken;locks optional.

### 4.4 ThreadLocal

new ThreadLocal();value accessor on proto;per-thr slot;initial undefined;any JS value;storage 5.8.

### 4.5 Atomics extended to (obj,propName)

Dispatch for every Atomics fn taking (typedArray,index,...):load,store,add,sub,and,or,xor,exchange,compareExchange,wait,waitAsync,notify. Placement:steps 0-3 go in the SHARED helpers (atomicReadModifyWrite(globalObject,vm,args,Func) AO:182,atomicStore) so host fns AND DFG/FTL untyped operationAtomics* (:641-737) route through (wait/waitAsync/notify:host-only,no JIT op);tier-up can't change semantics.

0. !uJT()=>today's body,textually intact:if (!uJT()) { body } (I1);steps 1-3 don't exist.
1. arg0 JSArrayBufferView (any view w/ float types):path unchanged (I1),one carve-out - 1a. TA sync-wait gate (GPO),Atomics.wait only:isJSThreadCurrent() (7)=>throw TypeError ("Atomics.wait cannot be called from the current thread.") before body (G30;hist;lifted post-GIL;doc'd hazard:cross-Thread sync TA wait deadlock). I21.
2. Else arg0 obj=>prop path;arg1 via ToPropertyKey.
3. Else TypeError (as today).

Atomics.isLockFree/pause unchanged. Prop semantics (all SeqCst;one atomic step each,THREAD.md:5;"own data k"=must exist as own data prop else TypeError):

| Op | Semantics |
|---|---|
| load(o,k) | Reads own prop k;absent,accessor,or proto-chain-only=>TypeError ("Atomics.load: object has no own property");returns value. |
| store(o,k,v) | Sets own data k to v (creates if absent+extensible;TypeError if accessor,non-writable,non-extensible+absent);returns v. |
| exchange(o,k,v) | store but requires own data k;returns prior value. |
| compareExchange(o,k,exp,rep) | own data k;if SVZ(current,exp) stores rep;returns value read either way (SVZ;=== breaks NaN CAS loops). |
| add/sub/and/or/xor(o,k,v) | own data k w/ JS number (stored non-number throws);ToNumber(v) operand;and/or/xor:ToInt32 both,int32 result;add/sub:double;stores result,returns old value. |
| wait(o,k,exp,timeout?) | own data k. !SVZ(current,exp)=>"not-equal";else block (G11-gated;GIL released;5.6)=>"ok"/"timed-out" (TA small strings,G10);termination:Terminated (5.6)=>throwTerminationException(). |
| waitAsync(o,k,exp,timeout?) | Same checks;{async:false,value:"not-equal"} or {async:true,value:Promise},TA shape;"timed-out" on finite timeout (5.6). |
| notify(o,k,count?) | Wakes<=count (default Infinity) (o,k) wtrs;returns count woken;0 valid even if o lacks k. |

Waiter identity=(cell,uid) (Dev 3);prop/TA wtrs never cross-woken.

### 4.6 Thread & process lifecycle

1. Completion=fn returns/throws. Still u/JSL:drain shared VM queue once (GPO;post-GIL:own queue till empty),publish result+wake/settle joiners (F1/F5),clear owned Strongs (5.10),unregisterLite+setCurrent(nullptr)+tag clear (5.2;N8);release JSL;destroy lite (5.2);exit. Never waits for tkts.
2. Tkts outlive thrs (process-owned;DWT per 5.5);dead thr's tkt settles per 5.5 GIL relaxation (I12);never-satisfied tkt never settles (not error),MAY keep shell alive forever (=TA waitAsync infinite;ditto leaked release fn).
3. Pending tkts keep the shell alive as TA waitAsync does:liveness=5.5 addPendingWork at REGISTRATION (no shell edits). No implicit join;mid-fn teardown may terminate abruptly,no invariant covers it (8).

## 5. Data structures,lock ordering,fences

### 5.1 Native thr state

```
ThreadManager (process singleton,WLM-style)
- Lock m_lock // rank 1 (5.9)
- HashMap<uint16_t /*tid*/, Ref<ThreadState>> m_threads // SPAWNED only;lazy;main/embedder TSs NOT here (currentThreadState)
- Deque<uint16_t> m_freeTIDs // empty until Dev-10 rebias lands
- uint16_t m_nextTID = 1 // 0=main,0x7fff=notTTLTID

ThreadState : ThreadSafeRefCounted<ThreadState>
- uint16_t tid
- RefPtr<WTF::Thread> nativeThread // set for lazy TSs too;compared,never deref'd (5.7.2)
- enum class Phase : uint8_t { Running,Finished,Failed } (std::atomic;F1/F5)
- Strong<Unknown> result // create/clear u/JSL (5.10)
- Strong<Unknown> fnSlot // roots fn spawn->call (5.10)
- Vector<Strong<Unknown>> argSlots // args likewise (5.10)
- Box<Lock> joinLock /*rank 3 (5.9)*/; Condition joinCondition // F5
- Vector<Ref<AT>> asyncJoiners // u/joinLock (F5)
- HashMap<uint64_t, Strong<Unknown>> threadLocals // 5.8;owner-thr-only
- Strong<JSThread> jsThread // Thread.current (I5);5.10
- Lock inboxLock /*rank 3*/; Vector<Ref<AT>> inbox; bool inboxOpen // post-GIL tkt inbox (5.5);phase 1:inert
```

TID release:Dev 10 - no reissue pre-rebias (teardown erases m_threads entry u/m_lock);m_nextTID==0x7fff && m_freeTIDs empty=>RangeError at spawn.

Cells:JSThread=JSDestructibleObject+Ref<TS>;subspaceFor->destructibleObjectSpace() (G13);ditto JSLockObject (+Ref<NLS>),JSConditionObject,JSThreadLocalObject (+uint64_t key).

currentThreadState() (SOLE lookup):static WTF::ThreadSpecific<RefPtr<ThreadState>>. Spawned body installs TS before fn;main/embedder first access creates+installs lazy TS (tid 0;nativeThread set);distinct embedder thrs=>distinct TSs (identity=Ref<TS>/nativeThread,never tid;7). NEVER via m_threads (tid-0 collision).

F1:Phase release-stored after result Strong written;join() readers load-acquire first (redundant u/GIL).

F5 join/compl protocol (asyncJoiners u/joinLock):
- Compl (4.6.1;after F1,u/JSL):u/joinLock - Phase release-store,joinCondition.notifyAll(),swap asyncJoiners out;drop joinLock;settle moved tkts via 5.5 schedule.
- join():Phase acquire!=Running=>read result u/JSL (no locks). Else 4.1 self-join/G11 checks;DAL;lock joinLock;while Running joinCondition.wait (5.9(a3));unlock;DAL ends;u/JSL read result.
- asyncJoin():u/JSL tkt;u/joinLock -!=Running=>schedule settle,else append. No lost wakeup:store+re-check both u/joinLock.

### 5.2 GIL protocol (phase-1 only;deleted later)

GIL=JSLock (G7) of single shared VM. No new lock.

- Thread body=VMLite+tag setup (below),then JSLockHolder lock(vm),then fn (atom table+stack limits migrate,G6).
 - GCClient bracketing (heap §9):once attach/detachCurrentThread() land,attach post-JSL-acq,detach in compl seq pre-JSL-release. Phase 1:no calls;DAL satisfies release-access.
 - VMLite+butterfly-tag handshake (G35/G36;9.2-8;until merged+enabled:no-op). Spawn,BEFORE JSLockHolder (=vmstate 6.4.4;didAcquireLock installs nothing;hist):lite=makeUnique<VMLite>()->lite->tid=ts->tid->VMLiteRegistry::singleton().registerLite(*lite, vm) (vm=shared GIL VM;sole writer of lite->vm,6.5.1)->setCurrent(lite.get())->initializeButterflyTIDTagForCurrentThread() (P5;after setCurrent;CS3:before any JS). Teardown (4.6.1;vmstate N8):STILL u/final JSL:unregisterLite->setCurrent(nullptr)->clearButterflyTIDTagForCurrentThread() (registry lock leaf,5.9-legal);after JSL release:destroy lite;TID retired forever (Dev 10). TM sole TID allocator. useVMLite on whenever uJT() at INT (Dev 2);Phase-A microtask queue inert.
- Blocking primitives (join,contended hold,cond.wait,prop Atomics.wait) park inside DAL (G7)=the only yield points.
- Cooperative-only (Dev 9).

### 5.3 NLS (backing Lock)

Not hand-rolled (hist):

```
NLS : ThreadSafeRefCounted<NLS>
- WTF::Lock m_lock // rank 4 leaf (G22)
- std::atomic<WTF::Thread*> m_holder // sync holder;null if free/async-held;written by acquirer;others compare,never deref
- std::atomic<bool> m_asyncHeld // tkt-held (atomic only for lock.locked)
- Lock m_queueLock // rank 3 (protects next two)
- RefPtr<AT> m_asyncHolder // LIVE async hold's tkt;non-null iff m_asyncHeld
- Deque<Ref<AT>> m_asyncWaiters  // FIFO async acquirers (5.5a)
```

hold:recur check (m_holder==&Thread::currentSingleton()=>throw)->m_lock.tryLock();on failure G11-gate then DAL+m_lock.lock()->store m_holder=current (relaxed;lock-ordered)->fn->epilogue guard:m_holder==current? clear m_holder->m_lock.unlock()->pump (5.5a R) :skip all three (4.3(a) consumed hold;unlock=double-unlock,G22).

F2:no custom mutex fences (WTF::Lock=acquire/release);m_holder relaxed (lock-bracketed);m_asyncHeld release-store/acquire-load.

### 5.4 NCS (backing Condition)

```
NCS : ThreadSafeRefCounted
- Lock queueLock // rank 3
- Deque<Ref<CondWaiter>> waiters // FIFO
CondWaiter
- enum kind { Sync,Async }
- std::atomic<uint8_t> state // Waiting->Notified, flipped exactly once
- (Sync: parked via ParkingLot on &state; Async: Ref<AT>)
```

wait(lock) order:
1. Verify caller holds lock (5.3);u/queueLock (m_lock still held;exempt,5.9(f)) append CondWaiter (Waiting).
2. Release JS Lock (clear m_holder,m_lock.unlock(),pump 5.5a-R).
3. DAL (GIL).
4. ParkingLot::parkConditionally(&wtr->state,validate=[state.load()==Waiting],deadline).
5. On return:take queueLock,re-check state. Still Waiting=>remove self from wtrs (must be present),spurious (I9);Notified=>notify already dequeued us. Release queueLock;then,still in step-3's DAL scope,m_lock.tryLock(),on failure m_lock.lock() (no GIL held;no recur/G11 check);m_holder=current;only then end DAL scope (GIL reacq w/ m_lock held,5.9(e));return.

notify() per wtr,dequeued FIFO u/queueLock:set state=Notified (release) still u/queueLock (dequeued<=>flipped,atomic vs step-5 re-check),then ParkingLot::unparkOne(&wtr->state).

F3:lost-wakeup guard=park-side validation (G22)+steps 1-2 enqueue-before-JS-lock-release (I9);hist.

Async wtrs:notify dequeues u/queueLock,then (not holding it) hands tkt to 5.5a A-failure path;never two rank-3 locks (5.9).

### 5.5 Async tkts

One tkt type for asyncJoin/asyncHold/asyncWait/prop waitAsync (WLM model,G10):{ Strong<JSPromise>,VM&,Ref<TS> registrant (4.6.2),DWT::Ticket dwtTicket,std::atomic<uint8_t> state /*Waiting->Notified|TimedOut*/,std::atomic<bool> consumed }. release()+cond.asyncWait consumption both CAS consumed;loser throws 4.2 Error. DWT protocol (=WLM's):registration u/JSL:dwtTicket=vm.deferredWorkTimer->addPendingWork(AtSomePoint,vm,promiseCell,{}) (WLM.cpp:67)=shell liveness (I20/4.6.3),NOT settle-time;settle:scheduleWorkSoon(dwtTicket,task) (:287) settles+clears Strong;never-settled:DWT VM-shutdown cancelPendingWork(VM&) (DWT.h:87);api adds no hook.

- GIL phase (relaxation,I12):one shared VM queue;settle on whichever thr drains it.
- Post-GIL surface:per-TS tkt inbox (5.1)+RL-wakeup hook. Settler never enqueues into another's MicrotaskQueue (vmstate I11):u/owner's inboxLock,inboxOpen=>append+wake owner RL (owner drains into own queue);else append to main TS inbox (compl seq closes inbox u/inboxLock,drains residue to main).

### 5.5a Async lock acquisition

Grants go to tkt,not parked thr (retry-on-grant,no direct handoff):

- A (acquire):asyncHold->m_lock.tryLock(). Success:u/QL set m_asyncHeld=true,m_asyncHolder=tkt,settle (5.5 task;with-fn arity:task runs fn,then E,then settles;I12). Failure:u/QL enqueue FIFO on m_asyncWaiters AND schedPump.
- schedPump (u/QL):pump-pending false=>set true,dispatch P on head tkt's vm.runLoop() (G28;<=1 pump/lock). NEVER DWT (tickets one-shot;hist);dwtTicket used once=final settle (5.5).
- R (release pump):every m_lock release (sync hold exit,async release(),E,cond.wait-2,4.3 release),after unlocking,takes QL;m_asyncWaiters non-empty=>schedPump.
- P (pump task,RL turn;GI):u/QL clear pump-pending FIRST;drop QL;m_lock.tryLock(). Success:u/QL;m_asyncWaiters empty (reachable;interleaving:hist)=>m_lock.unlock(),return (empty=>R no-op);else dequeue head,set m_asyncHeld/m_asyncHolder,settle. Failure:no action (holder's release runs R w/ pump-pending false,reschedules). Clear-before-tryLock normative (hist). Async starvation possible under perpetual sync contention;no livelock.
- Async release:release() CASes its tkt's consumed false->true (failure=>4.2 Error);u/QL assert m_asyncHolder==tkt,clear both;unlock m_lock (any thr;legal,hist);run R. 4.3(b) consumption=same sequence on m_asyncHolder's tkt.
- E (with-fn implicit release,4.2):post-fn (same RL task) CAS tkt consumed false->true. Success:u/QL assert m_asyncHolder==tkt,clear both;unlock;run R. Failure (4.3(b) consumed it):skip unlock+R,not an error. Either way settle w/ fn's result/exc (I23).
- cond.asyncWait reacq:on notify,enqueue via A's failure path (incl. schedPump);competes FIFO.

### 5.6 Property-wtr table (prop Atomics.wait/waitAsync/notify)

```
PropertyWaiterTable (process singleton,runtime/ThreadAtomics.cpp)
- Lock m_lock // rank 2
- HashMap<std::pair<JSCell*, UniquedStringImpl*>, Ref<PropertyWaiterList>> m_lists
  // per-list:Lock listLock (rank 3)+Deque of wtrs;mirrors WLM (G10);do NOT extend WLM (not owned)
```

Sync wtr:own WTF::Condition+std::atomic<uint8_t> state {Waiting,Notified,TimedOut,Terminated},flipped once,always u/LL (one flip arbitrates outcome). Liveness:non-empty list holds Strong<JSObject>+Ref<UniquedStringImpl> (waited-on obj GC-protected);entry removed when last wtr leaves (5.10).

5.6 is GPO (happens-before=JSL);post-GIL re-freeze (Dev 12):obj-model atomic reads,arming re-homed to owner inbox (5.5),4.5-1a gate lifted.

F4;Atomics.wait(o,k,exp,timeout) order:

1. u/JSL:validate per 4.5;read v=o.k;!SVZ(v,exp)=>"not-equal". No re-read below.
2. Still u/JSL:m_lock (rank 2)->find-or-create (cell,uid) list,first-wtr Strongs (5.10),drop;LL (rank 3)->enqueue Waiting,drop. JSL held from step-1 read through enqueue=lost-wakeup closure (hist);no LL across GIL drop.
3. DAL (no other lock held here).
4. Take LL. Loop (=WLM.cpp:86):exit if state!=Waiting||vm.hasTerminationRequest()||past deadline;else condition.waitUntil(listLock,min(deadline,now+10ms)) (releases LL while sleeping,G10),re-loop. 10ms quantum:poll mandatory - VMTraps can't wake PWT wtrs (pokes only vm.syncWaiter;9.2 forbids VMTraps edits).
5. Decide in same LL section:Notified=>"ok";else findAndRemove(self) (must succeed),then vm.hasTerminationRequest()=>set Terminated,else set TimedOut ("timed-out");record bool listNowEmpty.
6. Release LL;only then DAL scope ends+GIL reacq (5.9(e)). Nesting:steps 3-6=one DAL scope,Locker{listLock} strictly inside.
7. u/JSL again:if listNowEmpty,m_lock then LL in rank order,re-check,still empty=>remove entry+clear Strongs (5.10). state Terminated=>throwTerminationException() (4.5);else return TA small strings (G10).

notify(o,k,count) order:u/JSL (host call),take LL;dequeue<=count FIFO;sync:state=Notified then condition.notifyOne(),all u/LL;async:flip tkt Notified u/LL,collect. Release LL;settle collected tkts via 5.5 (never u/LL). Returns count flipped.

waitAsync tkts+timeout (GPO):tkts enqueue u/LL like sync wtrs (steps 1-2 u/JSL). Finite timeout arms at registration:vm.runLoop().dispatchAfter (G28),never RunLoop::currentSingleton() (G26). Timer task (VM RL):JSLockHolder (5.9(e)),then LL;Waiting=>findAndRemove,TimedOut,release LL,settle "timed-out" via 5.5 (cleanup per step 7);Notified=>release LL,no-op. Infinite:no timer.

### 5.7 Thread.restrict enforcement

Existing public mechanisms only;zero new Structure/TypeInfo machinery (G19/G20;hist).

1. Thread.restrict(o) (u/GIL;defeats+pins off fast paths;indexed GET unenforced,Dev 8). Sequence (after Dev-8/11 checks):(0) affinity hit:owner==caller=>return o (4.1 idempotency),else CAE. (a) o->ensureArrayStorage(vm) (JSObject.h:897/.cpp:1986-2025;legal for blank/non-array;non-null post-Dev-11;no-op on any ArrayStorage). (b) if (!hasSlowPutArrayStorage(o->indexingType())) o->switchToSlowPutArrayStorage(vm). Guard mandatory:SlowPut shapes CRASH() at :2060-2101 (no SlowPut case;reachable:restrict-after-bad-time,(a) no-ops there). (c) if (!o->structure()->isUncacheableDictionary()) o->convertToUncacheableDictionary(vm) (G21;keeps indexing mode). (d) setHasBeenFlattenedBefore(true) (G25);assert isUncacheableDictionary()&&hasSlowPutArrayStorage(indexingType()). SlowPut sticky=>all later indexed PUTs (incl. owner-added o[0]) stay on hooked generic paths. Pin mandatory (else first cache attempt re-flattens);bit inherited. Residual escapes=Dev-8 excluded receivers;corpus must not $vm.flattenDictionaryObject restricted objs;hasBeenDictionary() rejected (G32;hist).
2. Affinity table:process-singleton ThreadAffinityTable (ThreadManager.cpp):Lock rank 2+HashMap<JSCell*,Ref<TS> /*owner*/>+std::atomic<size_t> m_restrictedCount;entries pruned by per-insert Weak<JSObject> finalizers. Owner identity=restricting thr's Ref<TS>,never TID;threadRestrictCheck compares entry->owner->nativeThread.get()==&WTF::Thread::currentSingleton(). Export per 7. Mandatory fast path:relaxed m_restrictedCount load,zero=>true,no lock. Callers gate per 5.7.3 hook condition (flag-off=dead code).
3. Choke-point hook=9.2 entry 6,INTEGRATOR-applied after obj-model diff. Hook text:every generic-path entry point of Dev 8's enforced set - getOwnPropertySlotImpl (JSObject.h:1459),putInline*/putInlineSlow family (JSObjectInlines.h),putByIndex,deleteProperty+deletePropertyByIndex,defineOwnProperty,getOwnPropertyNames,setPrototype(Of),isExtensible,preventExtensions (JSObject.cpp);plus any successor generic entry point in the merged tree;MUST begin with
   `if (Options::useJSThreads() && structure->isUncacheableDictionary() && !threadRestrictCheck(globalObject, object)) [[unlikely]] return /*op-appropriate failure*/;`
 ([[unlikely]] per JSObject.cpp:528 idiom.)
 Get-path entry points (PropertySlot&) also skip on slot.isVMInquiry() (G31);uJT() guard mandatory (I1/I19).

### 5.8 ThreadLocal storage

Each JSThreadLocalObject carries process-unique monotonic uint64_t key (TM,u/m_lock). Storage=HashMap<uint64_t,Strong<Unknown>> in current TS (currentThreadState);get/set touch only that map (lock-free). Values root till thr exit/overwrite. Dead ThreadLocal cell leaks slots in live thrs till exit;doc'd (I13).

### 5.9 Lock ordering (total;against-rank acquisition=bug except (f))

```
rank 0:  GIL (JSLock) - outermost;dropped before any park
rank 1:  ThreadManager::m_lock
rank 2:  PWT::m_lock, ThreadAffinityTable lock (never both at once)
rank 3:  NCS::queueLock, NLS::m_queueLock,
         PropertyWaiterList::listLock, TS inbox lock, TS::joinLock (never two at once)
rank 4:  NLS::m_lock (WTF::Lock)/ParkingLot internal - leaf
```

(a) never indefinitely block holding any rank>=1 native lock;exemptions:(a1) parkConditionally validation u/ParkingLot internal lock (G22);(a2) 5.6-4 per-wtr WTF::Condition releases LL while sleeping (G10/G27),GIL dropped;(a3) F5 joinCondition.wait releases joinLock likewise. No other block-while-holding.
(b) GIL always released (DAL) before parking/blocking (5.4/5.6);
(c) WLM internal lock never held while taking any of above;
(d) never hold two rank-3 locks at once;
(e) GIL (rank 0) never (re)acquired holding any rank 1-3 lock;every 5.4/5.6 wake/timeout path releases all rank 1-3 locks before its DAL scope ends. One permitted rank-4-leaf shape:NLS::m_lock held across GIL reacq (5.3 contended hold;5.4-5;4.3/5.5a). Deadlock-freedom:hist.
(f) against-rank exemption:w/ NLS::m_lock (rank 4) held MAY take QL (5.3 pump,5.5a A/P/E/release) or NCS::queueLock (5.4-1;no cycle:notify never blocks on m_lock);legality:hist. Ranks not swapped ((e) needs the rank-4 leaf).

### 5.10 Strong<> handle lifecycle

Strong create/clear needs API lock. Each Strong created AND cleared only on a thr holding JSL,at:

| Strong | created | cleared |
|---|---|---|
| TS::jsThread (5.1) | spawner,u/GIL,pre Thread::create;lazy TS:first Strong (hook below) | spawned compl seq;main/embedder:~VM |
| TS result | compl seq | read u/JSL by settles;cleared by finalizer hook below (sole clearer) |
| TS::fnSlot+argSlots (spawn->run UAF) | spawner,u/GIL,pre Thread::create | right after fn returns/throws,pre-4.6.1 drain |
| TS::threadLocals values | setter (owner) | overwrite/thr-exit compl (owner);main:~VM |
| AT::Strong<JSPromise> | registration (host call) | settle (DWT task);never-settled:DWT VM-shutdown (5.5) |
| PWT cell Strongs | first-wtr insert (5.6-2,pre-DAL) | empty-list cleanup 5.6-7/waitAsync settle (rank order,re-check);timed-out dequeue touches no Strong |

~ThreadState RELEASE_ASSERTs result,fnSlot,argSlots,threadLocals,jsThread empty.

Finalizer hook (no VM.h/.cpp edit;EVERY TS,spawned+lazy):at TS::jsThread creation (spawner u/GIL pre Thread::create,or first lazy-TS Strong),register ONE vm.heap.addFinalizer(jsThread cell,lambda) (Heap.h:392-395);lambda holds Ref<TS>,clears any still-set jsThread/threadLocals/result Strongs. Finalizers run at GC finalize/lastChanceToFinalize() in ~VM (VM.cpp:633),thr holds JSL=>rule satisfied. Lazy TS:Strong pins cell=>fires in ~VM ("VM teardown"). Spawned TS:compl seq clears jsThread;hook fires at cell death or ~VM=SOLE clearer of TS::result. Early embedder-thr exit:TLS dtor drops only the RefPtr;lambda's Ref keeps TS till ~VM.

## 6. Invariants (numbered,testable)

>=1 §8 test cites each as API-I<n> (I24 termination half skippable);CI greps coverage.

I1:flag off (default)=>byte-identical to base for THIS WS's files (composed bar:other WSs' unconditional deltas excluded,vmstate R3/jit D7);flag on=>TA-arg0 Atomics.* identical incl. errors except I21;nothing else differs.
I2:any v (objs,NaN,-0):new Thread(() => v).join() SameValue-equal (objs:same ref).
I3:thrown e=>join() rethrows same e (identity);asyncJoin() rejects with it.
I4:all join/asyncJoin calls agree;none hangs post-compl.
I5:spawned-thr Thread.current ref-equal to parent's new Thread(...);stable.
I6:N x M lock.hold(() => counter++) on shared prop=>exactly N*M,main thr contending. GI.
I7:hold(fn) releases on throw;another thr's later hold succeeds.
I8:nested same-thr hold (incl. main) throws Error,no deadlock,outer hold kept.
I9:cond.wait enqueued before same-lock notify() is woken by it (spurious wakeups only add returns);producer/consumer+>=3-thr 2-wtr tests.
I10:notify(o,k) wakes parked wtr that observed SVZ(o[k],exp);no lost store+notify window (F4);ping-pong terminates.
I11:(o,"k") wtrs unaffected by notify on TA,(o,"j"),or another obj's "k".
I12:5.5 promises settle on RL turn,never sync in registering call (GPO:settling thr unspecified;post-GIL=registering thr,dead=>main,5.5).
I13:ThreadLocal writes invisible cross-thr;initial undefined everywhere;5.8 leak doc'd,not violation.
I14:after Thread.restrict(o) on T,every Dev-8 enforced op from thr!=T throws CAE (full named set;indexed set/delete/define on array;indexed set on plain {} after owner adds o[0]);T unaffected;values unchanged;survives 5.7.1 warm-ups;owner double-restrict returns o (5.7.1-0);post-bad-time (SlowPut) array restricts OK;Dev-8 unenforced set untested. INT gate via 9.2-6;//@ skipped until then.
I15:N x M Atomics.add(o,"x",1)=>exactly N*M (GI);ditto CAS retry loop.
I16:amplifier when present (G15):no prop-add vs read/write crash or lost Atomics.store-published prop;o.f === v reads only written values (THREAD.md:5).
I17:spawned ids in [1,0x7ffe];over maxJSThreads live OR lifetime TID exhaustion=>RangeError;ids unique;reissued only by Dev-10 rebias (5.1).
I18:G11-false thr:join(),contended hold(),cond.wait(),prop Atomics.wait throw TypeError;async variants+uncontended hold() succeed. Flip:--can-block-is-false (G34;per-VM=>all thrs G11-false u/GIL;hist);async paths never consult G11.
I19:flag off,bench-gate.sh passes vs integrator-recorded pre-WS baseline (G15;self-recorded=vacuous);INT gate. Implement:--record+gate same build exit 0.
I20:pending asyncJoin keeps shell alive till settle (4.6.3);finished thr's pending asyncHold continuation still settles (4.6.2).
I21:TA sync-wait gate (GPO):sync Atomics.wait on view from spawned Thread throws TypeError (4.5-1a),no park/side effects;main-thr calls+waitAsync/notify unchanged;deleted by re-freeze.
I22:prop Atomics.waitAsync,finite timeout,spawned thr,no notify=>settles "timed-out" (5.6 timer,G28);await from parent.
I23:asyncHold(fn) whose fn calls cond.asyncWait(lock):no Error,no double-unlock,settles w/ fn's result;later acquirers proceed (5.5a E).
I24:prop Atomics.wait:termination=>throwTerminationException (5.6-4),never "timed-out"/"ok";quantum wakeups never return spuriously.

## 7. Public interface for other WSs

Frozen signatures;bodies in 9.1 files;other WSs build against these.

```cpp
// runtime/ThreadManager.h
namespace JSC {

class ThreadManager {
public:
  JS_EXPORT_PRIVATE static ThreadManager& singleton();
  static constexpr uint16_t mainThreadTID = 0;
  static constexpr uint16_t notTTLTID = 0x7fff; // reserved
  static uint16_t currentTID(); // TID note below
  JS_EXPORT_PRIVATE static bool isJSThreadCurrent(); // true iff spawned Thread
  // diag/future N-mutator;NOT a GC root source:
  void forEachThreadState(const Invocable<void(ThreadState&)> auto&);
};

// currentButterflyTID():NOT here - sole provider vmstate 6.7 (ODR).

// 5.7 choke-point;true if allowed else throws CAE+returns false;
// callers gate on isUncacheableDictionary() first.
JS_EXPORT_PRIVATE bool threadRestrictCheck(JSGlobalObject*, JSObject*);

}

// runtime/ThreadAtomics.h - for DFG/FTL if it intrinsifies prop atomics
namespace JSC {
JS_EXPORT_PRIVATE JSValue atomicsLoadOnProperty(JSGlobalObject*, JSObject*, PropertyName);
JS_EXPORT_PRIVATE JSValue atomicsStoreOnProperty(JSGlobalObject*, JSObject*, PropertyName, JSValue);
JS_EXPORT_PRIVATE JSValue atomicsRMWOnProperty(JSGlobalObject*, JSObject*, PropertyName, AtomicsRMWOp, JSValue operand);
JS_EXPORT_PRIVATE JSValue atomicsCompareExchangeOnProperty(JSGlobalObject*, JSObject*, PropertyName, JSValue expected, JSValue replacement);
enum class AtomicsRMWOp : uint8_t { Add, Sub, And, Or, Xor, Exchange };
}

// runtime/ThreadObject.h (9.2-2 init() hunk):five
// JSValue createXXXProperty(VM&, JSObject* globalObject),XXX in
// {Thread,Lock,Condition,ThreadLocal,ConcurrentAccessError}.
```

TID note:currentTID()=0 on main+(GPO;GIL-serialized,sound) embedder thrs;post-GIL:real TID lazily at first VM entry. TIDs never lock-holder (5.3) nor restrict-owner (5.7.2) identity.

Type names (JSC::JSLock taken):JSThread,JSLockObject,JSConditionObject,JSThreadLocalObject;option names per 3;no ThreadRestricted TypeInfo flag.

## 8. Test corpus layout;JT/

Owned under JT/:9.1 list. NOT owned:bench/** (G15),heap-*.js,objectmodel/**,vmstate/**,jit/**.

harness.js:shouldBe,shouldThrow(type,fn),spawnN(n,fn),withTimeout(ms,fn).

Per-file manifests (api/, atomics/, races/ with API-I<n> mapping)=SPEC-api-annex.md §T, FROZEN NORMATIVE, verbatim.

threads.yaml:9.2-7,not created here.

Conventions=annex §T2,FROZEN NORMATIVE,verbatim. Tools/threads/run-tests.sh (owned,new):globs JT/{api,atomics,races}/*.js+threads/heap-*.js+threads/{objectmodel,vmstate}/*.js+threads/jit/**/*.js when present (not owned;vmstate N6:656-659;jit:5);honors JSC env var,--filter=,--amplify (wrap via Tools/threads/amplify.sh if present;else warn once,run plain).

## 9. File ownership and INT manifest

### 9.1 Owned paths (ONLY files this WS's implr may create/edit)

```
runtime/ThreadObject.h/.cpp # JSThread,ctors/protos,createXXXProperty,CAE
runtime/ThreadManager.h/.cpp # TS,TIDs,threadRestrictCheck,affinity table,TL keys (NOT currentButterflyTID,7)
runtime/ThreadAtomics.h/.cpp # prop Atomics+PWT
runtime/ThreadLocalObject.h/.cpp # JSThreadLocalObject
runtime/LockObject.h/.cpp # JSLockObject+NLS
runtime/ConditionObject.h/.cpp # JSConditionObject+NCS
runtime/AtomicsObject.cpp # dispatch split only (4.5 0-3);TA path intact (I1)
JSTests/threads/harness.js, api/**, atomics/**, races/**
Tools/threads/run-tests.sh
```

### 9.2 INTEGRATE-api.md manifest (shared hot files;implrs MUST NOT edit;verbatim)

1. runtime/OptionsList.h - the four §3 options,format of :638/680;NOTHING after a continuation backslash (hist):
   ```
   v(Bool, useJSThreads, false, Normal, "enable shared-memory Thread/Lock/Condition/ThreadLocal API"_s) \
   v(Unsigned, maxJSThreads, 32766, Normal, nullptr) \
   v(Unsigned, jsThreadGILTimeSliceMs, 0, Normal, "reserved, inert in phase 1 (SPEC-api Deviation 9)"_s) \
   v(Unsigned, jsThreadStackSizeKB, 0, Normal, nullptr) \
   ```
 Dedupe vs jit M1/objectmodel 10-1:ONE entry lands,this text canonical;no useConcurrentJS (G33;grep lint).
2. runtime/JSGlobalObject.cpp - SOLE mechanism (hist):in init() after the useSharedArrayBuffer block (G12):
   ```
   if (Options::useJSThreads()) { /* five putDirectWithoutTransition(vm,
      Identifier::fromString(vm, "XXX"_s), createXXXProperty(vm, this), DontEnum),
      XXX in {Thread, Lock, Condition, ThreadLocal, ConcurrentAccessError} */ }
   ```
 +#include "ThreadObject.h".
3. (Removed;no JSTypeInfo.h edit - 5.7 uses no TypeInfo.)
4. Sources.txt - six new runtime/*.cpp from 9.1 (alphabetical,near :765).
5. CMakeLists.txt - six new .h into JavaScriptCore_PRIVATE_FRAMEWORK_HEADERS.
6. Thread.restrict choke-point hook:5.7.3 text+entry list as exact diff vs JSObject.h/JSObjectInlines.h/JSObject.cpp (INTEGRATOR applies post-obj-model diff);this WS supplies threadRestrictCheck (7),conversions+pin (5.7.1),api/thread-restrict.js (I14).
7. Test-runner wiring:JSTests/threads.yaml (or run-javascriptcore-tests stanza) running api/,atomics/,races/+when present heap-*.js,objectmodel/,vmstate/ (N6),jit/ via runDefault-family;blocking-gate stanza:--can-block-is-false (8);until landed,run-tests.sh (8;same coverage) is runner.
8. VMLite+butterfly-tag wiring (5.2) in THIS WS's ThreadObject.cpp/ThreadManager.cpp:#includes+exact 5.2 spawn/teardown calls as ready-to-apply diff (headers absent now);INTEGRATOR applies VMLite part post-vmstate,tag part post-jit-1b;parts independent.

No edits to VM.h/.cpp (5.10 hook=public Heap API),JSGlobalObject.h,VMTraps.*,Structure.*,StructureTransitionTable.h,JSTypeInfo.h (rationale:hist).

## 10. Ordered task list (one implr)

1. Scaffolding:six 9.1 file pairs (skeletons,ClassInfo,protos,subspaceFor,five createXXXProperty);verify 9.2-1 pre-applied,overlay other 9.2 hunks (3);mirror into INTEGRATE-api.md as you go.
2. TM+GIL spawn:TS,TID alloc/recycling (5.1,I17),currentTID+currentThreadState,new Thread per 5.2 order w/ fnSlot/argSlots rooting,result capture (F1),compl seq (4.6.1/F5),sync join (F5;I2-I5,I18).
3. Thread.current+lazy main-thr state.
4. asyncJoin via 5.5 tkts (I12,I20).
5. Lock:NLS (5.3),hold (I6-I8);then 5.5a:asyncHold both arities,with-fn epilogue E,release pump,locked getter.
6. Condition:5.4 protocol,wait/notify/notifyAll (I9);asyncWait reacq via 5.5a.
7. ThreadLocal (5.8,I13).
8. Atomics dispatch split (4.5 0-3,in shared helpers),TA path textually intact,1a gate (I21);land ta-path-unchanged.js first,stay green (I1).
9. Property atomics:load/store/exchange/compareExchange (SVZ),then RMW family (I15).
10. PWT (5.6/F4;G28 timer):prop wait/waitAsync/notify incl. termination poll (I10,I11,I22,I24).
11. Thread.restrict+CAE (5.7):exclusions (Dev 8/11),conversions+pin (5.7.1),affinity table,threadRestrictCheck,hook diff (entry 6),warm-up tests (I14).
12. Test corpus:every §8 file;wire run-tests.sh;coverage grep API-I1..I24.
13. Gates (degrade gracefully;G15):races via run-tests.sh;TSAN no-JIT if target exists;bench-gate.sh --record+gate same build exit 0 (I19).
14. Finalize INTEGRATE-api.md w/ exact build-tested diffs (steps 1,8,11).
15. (post-GIL,chartered-owned w/ OM Task 13) TID rebias/reissue per Dev 10.
