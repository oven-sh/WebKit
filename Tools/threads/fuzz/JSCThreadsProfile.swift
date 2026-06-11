// Copyright 2019 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Profile for JavaScriptCore with the shared-memory Thread API (--useJSThreads).
// Extends the stock JSC profile with Thread/Lock/Condition/ThreadLocal builtins
// and code generators that stress cross-thread object-model interactions:
// shared-object property storms, array resize races, dictionary flips,
// Atomics on (object, propertyName), Thread.restrict, proxies/getters on
// shared objects.

// Small fixed set of property names so that concurrently running threads
// actually collide on the same (cell, uid) pairs.
private let sharedPropNames = ["a", "b", "c", "x", "y", "s0", "s1", "len"]

private func randomSharedPropName() -> String {
    return chooseUniform(from: sharedPropNames)
}

private let threadsForceDFGCompilationGenerator = CodeGenerator(
    "ThreadsForceDFGCompilationGenerator", inputs: .required(.function())
) { b, f in
    assert(b.type(of: f).Is(.function()))
    let arguments = b.randomArguments(forCalling: f)

    b.buildRepeatLoop(n: 10) { _ in
        b.callFunction(f, withArgs: arguments, guard: true)
    }
}

private let threadsForceFTLCompilationGenerator = CodeGenerator(
    "ThreadsForceFTLCompilationGenerator", inputs: .required(.function())
) { b, f in
    assert(b.type(of: f).Is(.function()))
    let arguments = b.randomArguments(forCalling: f)

    b.buildRepeatLoop(n: 100) { _ in
        b.callFunction(f, withArgs: arguments, guard: true)
    }
}

private let threadsGcGenerator = CodeGenerator("ThreadsGcGenerator") { b in
    b.callFunction(b.createNamedVariable(forBuiltin: "gc"))
}

// ILTypes for the Thread API surface.
private let jsThreadObjectType = ILType.object(
    ofGroup: "Thread", withProperties: ["id"], withMethods: ["join", "asyncJoin"])
private let jsThreadConstructorType =
    ILType.constructor([.function(), .opt(.jsAnything), .opt(.jsAnything)] => jsThreadObjectType)
    + .object(
        ofGroup: "ThreadConstructor", withProperties: ["current", "prototype"],
        withMethods: ["restrict"])
private let jsLockObjectType = ILType.object(
    ofGroup: "Lock", withProperties: ["locked"], withMethods: ["hold", "asyncHold"])
private let jsLockConstructorType = ILType.constructor([] => jsLockObjectType)
private let jsConditionObjectType = ILType.object(
    ofGroup: "Condition", withMethods: ["wait", "asyncWait", "notify", "notifyAll"])
private let jsConditionConstructorType = ILType.constructor([] => jsConditionObjectType)
private let jsThreadLocalObjectType = ILType.object(
    ofGroup: "ThreadLocal", withProperties: ["value"])
private let jsThreadLocalConstructorType = ILType.constructor([] => jsThreadLocalObjectType)

private let jsThreadGroup = ObjectGroup(
    name: "Thread",
    instanceType: jsThreadObjectType,
    properties: ["id": .number],
    methods: [
        "join": [] => .jsAnything,
        "asyncJoin": [] => .jsPromise,
    ])

private let jsThreadConstructorGroup = ObjectGroup(
    name: "ThreadConstructor",
    constructorPath: "Thread",
    instanceType: jsThreadConstructorType,
    properties: [
        "current": jsThreadObjectType,
        "prototype": .object(),
    ],
    methods: [
        "restrict": [.object()] => .object()
    ])

private let jsLockGroup = ObjectGroup(
    name: "Lock",
    instanceType: jsLockObjectType,
    properties: ["locked": .boolean],
    methods: [
        "hold": [.function()] => .jsAnything,
        "asyncHold": [.opt(.function())] => .jsPromise,
    ])

private let jsConditionGroup = ObjectGroup(
    name: "Condition",
    instanceType: jsConditionObjectType,
    properties: [:],
    methods: [
        "wait": [.plain(jsLockObjectType)] => .undefined,
        "asyncWait": [.plain(jsLockObjectType)] => .jsPromise,
        "notify": [] => .number,
        "notifyAll": [] => .number,
    ])

private let jsThreadLocalGroup = ObjectGroup(
    name: "ThreadLocal",
    instanceType: jsThreadLocalObjectType,
    properties: ["value": .jsAnything],
    methods: [:])

// --- Code generators ----------------------------------------------------

// Spawn a thread running a freshly built function over captured variables;
// usually join it so results flow back and programs stay bounded.
private let threadSpawnGenerator = CodeGenerator("ThreadSpawnGenerator") { b in
    let threadCtor = b.createNamedVariable(forBuiltin: "Thread")
    let numArgs = Int.random(in: 0...2)
    let fn = b.buildPlainFunction(with: .parameters(n: numArgs)) { _ in
        b.build(n: 5)
        b.doReturn(b.randomJsVariable())
    }
    var args: [Variable] = [fn]
    for _ in 0..<numArgs {
        args.append(b.randomJsVariable())
    }
    let thread = b.construct(threadCtor, withArgs: args, guard: true)
    if probability(0.75) {
        if probability(0.8) {
            b.callMethod("join", on: thread, guard: true)
        } else {
            b.callMethod("asyncJoin", on: thread, guard: true)
        }
    }
}

private let threadJoinGenerator = CodeGenerator(
    "ThreadJoinGenerator", inputs: .required(.object(ofGroup: "Thread"))
) { b, t in
    if probability(0.7) {
        b.callMethod("join", on: t, guard: true)
    } else {
        b.callMethod("asyncJoin", on: t, guard: true)
    }
}

// Two threads (plus the spawner) hammering the same object's named properties:
// add/read/write/delete storms.
private let sharedObjectPropertyStormGenerator = CodeGenerator(
    "SharedObjectPropertyStormGenerator", inputs: .required(.object())
) { b, obj in
    let threadCtor = b.createNamedVariable(forBuiltin: "Thread")
    let worker = b.buildPlainFunction(with: .parameters(n: 1)) { args in
        let o = args[0]
        b.buildRepeatLoop(n: Int.random(in: 20...200)) { i in
            b.setProperty(randomSharedPropName(), of: o, to: i)
            b.getProperty(randomSharedPropName(), of: o, guard: true)
            if probability(0.3) {
                b.deleteProperty(randomSharedPropName(), of: o, guard: true)
            }
            if probability(0.2) {
                b.setProperty(randomSharedPropName(), of: o, to: b.randomJsVariable())
            }
        }
    }
    let t1 = b.construct(threadCtor, withArgs: [worker, obj], guard: true)
    var t2: Variable? = nil
    if probability(0.5) {
        t2 = b.construct(threadCtor, withArgs: [worker, obj], guard: true)
    }
    // Storm from the spawning thread as well.
    b.buildRepeatLoop(n: Int.random(in: 20...100)) { i in
        b.setProperty(randomSharedPropName(), of: obj, to: i)
        b.getProperty(randomSharedPropName(), of: obj, guard: true)
        if probability(0.2) {
            b.deleteProperty(randomSharedPropName(), of: obj, guard: true)
        }
    }
    b.callMethod("join", on: t1, guard: true)
    if let t2 = t2 {
        b.callMethod("join", on: t2, guard: true)
    }
}

// Concurrent array resizing: one thread pushes/pops/writes length while
// the spawner reads and writes elements (indexing-shape races).
private let sharedArrayResizeRaceGenerator = CodeGenerator("SharedArrayResizeRaceGenerator") { b in
    let threadCtor = b.createNamedVariable(forBuiltin: "Thread")
    var initial: [Variable] = []
    for _ in 0..<Int.random(in: 0...5) {
        initial.append(b.randomJsVariable())
    }
    let arr = b.createArray(with: initial)
    let resizer = b.buildPlainFunction(with: .parameters(n: 1)) { args in
        let a = args[0]
        b.buildRepeatLoop(n: Int.random(in: 20...200)) { i in
            b.callMethod("push", on: a, withArgs: [i], guard: true)
            if probability(0.4) {
                b.callMethod("pop", on: a, guard: true)
            }
            if probability(0.2) {
                b.setProperty("length", of: a, to: b.loadInt(Int64.random(in: 0...64)))
            }
            if probability(0.2) {
                // Sparse write: may force ArrayStorage / shape changes.
                b.setElement(Int64.random(in: 0...10000), of: a, to: i)
            }
        }
    }
    let t = b.construct(threadCtor, withArgs: [resizer, arr], guard: true)
    b.buildRepeatLoop(n: Int.random(in: 20...100)) { i in
        b.setElement(Int64.random(in: 0...32), of: arr, to: i)
        b.getElement(Int64.random(in: 0...32), of: arr, guard: true)
        b.getProperty("length", of: arr)
    }
    b.callMethod("join", on: t, guard: true)
}

// Flip an object into (uncacheable) dictionary mode through deletes and
// Thread.restrict, while another thread touches it.
private let dictionaryFlipGenerator = CodeGenerator(
    "DictionaryFlipGenerator", inputs: .required(.object())
) { b, obj in
    let threadCtor = b.createNamedVariable(forBuiltin: "Thread")
    // Bulk-add then delete properties to push toward dictionary mode.
    for name in sharedPropNames {
        b.setProperty(name, of: obj, to: b.randomJsVariable())
    }
    let toucher = b.buildPlainFunction(with: .parameters(n: 1)) { args in
        let o = args[0]
        b.buildRepeatLoop(n: Int.random(in: 10...100)) { _ in
            b.getProperty(randomSharedPropName(), of: o, guard: true)
            b.setProperty(randomSharedPropName(), of: o, to: b.randomJsVariable())
        }
    }
    let t = b.construct(threadCtor, withArgs: [toucher, obj], guard: true)
    for name in sharedPropNames where probability(0.5) {
        b.deleteProperty(name, of: obj, guard: true)
    }
    if probability(0.5) {
        // Thread.restrict flips to uncacheable dictionary + SlowPut storage;
        // cross-thread touches must raise ConcurrentAccessError, not crash.
        b.callMethod("restrict", on: threadCtor, withArgs: [obj], guard: true)
    }
    b.callMethod("join", on: t, guard: true)
}

// Thread.restrict from a spawned thread, then violate the restriction from
// the spawner and (maybe) a third thread.
private let threadRestrictGenerator = CodeGenerator(
    "ThreadRestrictGenerator", inputs: .required(.object())
) { b, obj in
    let threadCtor = b.createNamedVariable(forBuiltin: "Thread")
    let restrictor = b.buildPlainFunction(with: .parameters(n: 1)) { args in
        let o = args[0]
        b.callMethod("restrict", on: threadCtor, withArgs: [o], guard: true)
        b.buildRepeatLoop(n: Int.random(in: 5...50)) { i in
            b.setProperty(randomSharedPropName(), of: o, to: i)
        }
    }
    let t = b.construct(threadCtor, withArgs: [restrictor, obj], guard: true)
    // These accesses race with the restrict; once it lands they must throw
    // ConcurrentAccessError (guarded), never corrupt state.
    b.buildRepeatLoop(n: Int.random(in: 5...50)) { i in
        b.setProperty(randomSharedPropName(), of: obj, to: i, guard: true)
        b.getProperty(randomSharedPropName(), of: obj, guard: true)
        b.deleteProperty(randomSharedPropName(), of: obj, guard: true)
    }
    b.callMethod("join", on: t, guard: true)
}

// Atomics.* on (object, propertyName) pairs, cross-thread.
private let propertyAtomicsGenerator = CodeGenerator(
    "PropertyAtomicsGenerator", inputs: .required(.object())
) { b, obj in
    let atomics = b.createNamedVariable(forBuiltin: "Atomics")
    let threadCtor = b.createNamedVariable(forBuiltin: "Thread")
    let key = b.loadString(randomSharedPropName())
    // Seed the property so own-data-property requirements are usually met.
    b.callMethod("store", on: atomics, withArgs: [obj, key, b.loadInt(0)], guard: true)

    let rmwOps = ["add", "sub", "and", "or", "xor", "exchange"]
    let worker = b.buildPlainFunction(with: .parameters(n: 1)) { args in
        let o = args[0]
        b.buildRepeatLoop(n: Int.random(in: 20...200)) { _ in
            let op = chooseUniform(from: rmwOps)
            b.callMethod(op, on: atomics, withArgs: [o, key, b.loadInt(1)], guard: true)
            if probability(0.3) {
                b.callMethod("load", on: atomics, withArgs: [o, key], guard: true)
            }
            if probability(0.2) {
                b.callMethod(
                    "compareExchange", on: atomics,
                    withArgs: [o, key, b.loadInt(Int64.random(in: 0...8)), b.loadInt(0)],
                    guard: true)
            }
            if probability(0.1) {
                // Plain (non-atomic) write racing the atomic ops.
                b.setProperty(randomSharedPropName(), of: o, to: b.randomJsVariable())
            }
        }
    }
    let t = b.construct(threadCtor, withArgs: [worker, obj], guard: true)
    b.buildRepeatLoop(n: Int.random(in: 20...100)) { _ in
        b.callMethod(
            chooseUniform(from: rmwOps), on: atomics, withArgs: [obj, key, b.loadInt(1)],
            guard: true)
    }
    b.callMethod("join", on: t, guard: true)
}

// Atomics.wait/waitAsync/notify on properties: waiter thread with a bounded
// timeout, notifier on the spawning thread.
private let propertyAtomicsWaitNotifyGenerator = CodeGenerator(
    "PropertyAtomicsWaitNotifyGenerator", inputs: .required(.object())
) { b, obj in
    let atomics = b.createNamedVariable(forBuiltin: "Atomics")
    let threadCtor = b.createNamedVariable(forBuiltin: "Thread")
    let key = b.loadString(randomSharedPropName())
    b.callMethod("store", on: atomics, withArgs: [obj, key, b.loadInt(0)], guard: true)

    let waiter = b.buildPlainFunction(with: .parameters(n: 1)) { args in
        let o = args[0]
        if probability(0.5) {
            // Bounded sync wait (5-50ms) so programs terminate.
            b.callMethod(
                "wait", on: atomics,
                withArgs: [o, key, b.loadInt(0), b.loadInt(Int64.random(in: 5...50))],
                guard: true)
        } else {
            b.callMethod(
                "waitAsync", on: atomics,
                withArgs: [o, key, b.loadInt(0), b.loadInt(Int64.random(in: 5...50))],
                guard: true)
        }
        b.callMethod("load", on: atomics, withArgs: [o, key], guard: true)
    }
    let t = b.construct(threadCtor, withArgs: [waiter, obj], guard: true)
    b.buildRepeatLoop(n: Int.random(in: 5...50)) { _ in
        b.callMethod("store", on: atomics, withArgs: [obj, key, b.loadInt(1)], guard: true)
        b.callMethod(
            "notify", on: atomics, withArgs: [obj, key, b.loadInt(Int64.random(in: 0...4))],
            guard: true)
    }
    b.callMethod("join", on: t, guard: true)
}

// Lock.hold contention across threads guarding shared-object mutation.
private let lockContentionGenerator = CodeGenerator(
    "LockContentionGenerator", inputs: .required(.object())
) { b, obj in
    let threadCtor = b.createNamedVariable(forBuiltin: "Thread")
    let lockCtor = b.createNamedVariable(forBuiltin: "Lock")
    let lock = b.construct(lockCtor)
    let worker = b.buildPlainFunction(with: .parameters(n: 2)) { args in
        let l = args[0]
        let o = args[1]
        b.buildRepeatLoop(n: Int.random(in: 10...100)) { i in
            let critical = b.buildPlainFunction(with: .parameters(n: 0)) { _ in
                b.setProperty(randomSharedPropName(), of: o, to: i)
                b.getProperty(randomSharedPropName(), of: o, guard: true)
            }
            if probability(0.85) {
                b.callMethod("hold", on: l, withArgs: [critical], guard: true)
            } else {
                b.callMethod("asyncHold", on: l, withArgs: [critical], guard: true)
            }
        }
    }
    let t1 = b.construct(threadCtor, withArgs: [worker, lock, obj], guard: true)
    let t2 = b.construct(threadCtor, withArgs: [worker, lock, obj], guard: true)
    // Contend from the spawner too, with unguarded raw access mixed in.
    b.buildRepeatLoop(n: Int.random(in: 10...50)) { i in
        let critical = b.buildPlainFunction(with: .parameters(n: 0)) { _ in
            b.setProperty(randomSharedPropName(), of: obj, to: i)
        }
        b.callMethod("hold", on: lock, withArgs: [critical], guard: true)
        if probability(0.3) {
            b.getProperty("locked", of: lock)
        }
    }
    b.callMethod("join", on: t1, guard: true)
    b.callMethod("join", on: t2, guard: true)
}

// Condition wait/notify across threads (producer/consumer-ish). Waits can
// time out the whole program if a wakeup is lost; keep notify storms large.
private let conditionWaitNotifyGenerator = CodeGenerator("ConditionWaitNotifyGenerator") { b in
    let threadCtor = b.createNamedVariable(forBuiltin: "Thread")
    let lockCtor = b.createNamedVariable(forBuiltin: "Lock")
    let condCtor = b.createNamedVariable(forBuiltin: "Condition")
    let lock = b.construct(lockCtor)
    let cond = b.construct(condCtor)
    let flag = b.createObject(with: ["done": b.loadInt(0)])

    let waiterFn = b.buildPlainFunction(with: .parameters(n: 3)) { args in
        let l = args[0]
        let c = args[1]
        let f = args[2]
        let critical = b.buildPlainFunction(with: .parameters(n: 0)) { _ in
            // Predicate loop per spec; bounded to avoid infinite waits when
            // the notifier already finished.
            b.buildRepeatLoop(n: 3) { _ in
                b.callMethod("wait", on: c, withArgs: [l], guard: true)
                b.getProperty("done", of: f, guard: true)
            }
        }
        b.callMethod("hold", on: l, withArgs: [critical], guard: true)
    }
    let t = b.construct(threadCtor, withArgs: [waiterFn, lock, cond, flag], guard: true)
    b.setProperty("done", of: flag, to: b.loadInt(1))
    b.buildRepeatLoop(n: 200) { _ in
        if probability(0.7) {
            b.callMethod("notifyAll", on: cond, guard: true)
        } else {
            b.callMethod("notify", on: cond, guard: true)
        }
    }
    b.callMethod("join", on: t, guard: true)
}

// ThreadLocal values diverging across threads.
private let threadLocalGenerator = CodeGenerator("ThreadLocalGenerator") { b in
    let threadCtor = b.createNamedVariable(forBuiltin: "Thread")
    let tlCtor = b.createNamedVariable(forBuiltin: "ThreadLocal")
    let tl = b.construct(tlCtor)
    b.setProperty("value", of: tl, to: b.randomJsVariable())
    let worker = b.buildPlainFunction(with: .parameters(n: 1)) { args in
        let slot = args[0]
        b.getProperty("value", of: slot, guard: true)
        b.setProperty("value", of: slot, to: b.randomJsVariable())
        b.getProperty("value", of: slot, guard: true)
    }
    let t = b.construct(threadCtor, withArgs: [worker, tl], guard: true)
    b.getProperty("value", of: tl, guard: true)
    b.callMethod("join", on: t, guard: true)
}

// Proxy / accessor traps on objects shared across threads.
private let sharedProxyGetterGenerator = CodeGenerator(
    "SharedProxyGetterGenerator", inputs: .required(.object())
) { b, obj in
    let threadCtor = b.createNamedVariable(forBuiltin: "Thread")
    // Install an accessor that mutates the target from inside the getter.
    b.eval(
        """
        (function(o) {
            try {
                Object.defineProperty(o, 'acc', {
                    configurable: true,
                    get() { o.x = (o.x | 0) + 1; return o.x; },
                    set(v) { delete o.y; o.y = v; }
                });
            } catch {}
        })(%@)
        """, with: [obj])
    let proxy = b.eval(
        """
        new Proxy(%@, {
            get(t, k, r) { try { t.b = k; } catch {} return Reflect.get(t, k, r); },
            set(t, k, v, r) { try { delete t.c; } catch {} return Reflect.set(t, k, v, r); },
            has(t, k) { return Reflect.has(t, k); },
            deleteProperty(t, k) { return Reflect.deleteProperty(t, k); }
        })
        """, with: [obj], hasOutput: true)!
    let worker = b.buildPlainFunction(with: .parameters(n: 1)) { args in
        let p = args[0]
        b.buildRepeatLoop(n: Int.random(in: 10...100)) { i in
            b.getProperty("acc", of: p, guard: true)
            b.setProperty("acc", of: p, to: i, guard: true)
            b.getProperty(randomSharedPropName(), of: p, guard: true)
            b.setProperty(randomSharedPropName(), of: p, to: i, guard: true)
        }
    }
    let t = b.construct(threadCtor, withArgs: [worker, proxy], guard: true)
    b.buildRepeatLoop(n: Int.random(in: 10...50)) { i in
        b.getProperty("acc", of: obj, guard: true)
        b.setProperty(randomSharedPropName(), of: obj, to: i)
    }
    b.callMethod("join", on: t, guard: true)
}

// Make a thread run a JIT-warm function over a shared object, racing shape
// changes from the spawner (IC vs transition races).
private let crossThreadJITWarmupGenerator = CodeGenerator(
    "CrossThreadJITWarmupGenerator", inputs: .required(.object())
) { b, obj in
    let threadCtor = b.createNamedVariable(forBuiltin: "Thread")
    let hot = b.buildPlainFunction(with: .parameters(n: 1)) { args in
        let o = args[0]
        b.getProperty(randomSharedPropName(), of: o, guard: true)
        b.setProperty(randomSharedPropName(), of: o, to: b.loadInt(1))
    }
    let driver = b.buildPlainFunction(with: .parameters(n: 2)) { args in
        let f = args[0]
        let o = args[1]
        b.buildRepeatLoop(n: 200) { _ in
            b.callFunction(f, withArgs: [o], guard: true)
        }
    }
    let t = b.construct(threadCtor, withArgs: [driver, hot, obj], guard: true)
    // Shape-shift underneath the hot loop.
    b.buildRepeatLoop(n: Int.random(in: 10...100)) { i in
        b.setProperty(randomSharedPropName(), of: obj, to: i)
        if probability(0.3) {
            b.deleteProperty(randomSharedPropName(), of: obj, guard: true)
        }
        if probability(0.1) {
            let proto = b.createObject(with: [randomSharedPropName(): i])
            let objectBuiltin = b.createNamedVariable(forBuiltin: "Object")
            b.callMethod("setPrototypeOf", on: objectBuiltin, withArgs: [obj, proto], guard: true)
        }
    }
    b.callMethod("join", on: t, guard: true)
}

let jscThreadsProfile = Profile(
    processArgs: { randomize in
        var args = [
            "--validateOptions=true",
            // No need to call functions thousands of times before they are JIT compiled.
            "--thresholdForJITSoon=10",
            "--thresholdForJITAfterWarmUp=10",
            "--thresholdForOptimizeAfterWarmUp=100",
            "--thresholdForOptimizeAfterLongWarmUp=100",
            "--thresholdForOptimizeSoon=100",
            "--thresholdForFTLOptimizeAfterWarmUp=1000",
            "--thresholdForFTLOptimizeSoon=1000",
            "--validateBCE=true",
            // The whole point of this profile.
            "--useJSThreads=true",
            "--reprl",
        ]

        guard randomize else { return args }

        // Stock JSC tier rotation.
        args.append("--useBaselineJIT=\(probability(0.9) ? "true" : "false")")
        args.append("--useDFGJIT=\(probability(0.9) ? "true" : "false")")
        args.append("--useFTLJIT=\(probability(0.9) ? "true" : "false")")
        args.append("--useAccessInlining=\(probability(0.9) ? "true" : "false")")
        args.append("--useObjectAllocationSinking=\(probability(0.9) ? "true" : "false")")

        // Threads-specific stress rotation.
        // Kill switches for the threaded IC tiers.
        args.append("--useThreadedLLIntICs=\(probability(0.85) ? "true" : "false")")
        args.append("--useThreadedBaselineICs=\(probability(0.85) ? "true" : "false")")
        args.append("--useThreadedDFG=\(probability(0.85) ? "true" : "false")")
        args.append("--useThreadedFTL=\(probability(0.85) ? "true" : "false")")
        // Concurrent object-model stress modes.
        if probability(0.25) {
            args.append("--forceSegmentedButterflies=true")
        }
        if probability(0.25) {
            args.append("--forceButterflySWBit=true")
        }
        if probability(0.25) {
            args.append("--verifyConcurrentButterfly=true")
        }
        if probability(0.1) {
            args.append("--validateButterflyTagDiscipline=true")
        }
        if probability(0.25) {
            args.append("--useStructureAllocationLock=true")
        }
        if probability(0.2) {
            args.append("--jsThreadStackSizeKB=\(chooseUniform(from: [256, 512, 1024]))")
        }

        return args
    },

    processArgsReference: nil,

    processEnv: ["UBSAN_OPTIONS": "handle_segv=0"],

    // Threads leak across executions more easily than plain JS state;
    // respawn more often than the stock JSC profile.
    maxExecsBeforeRespawn: 100,

    // Joins, lock contention, and bounded Atomics.wait calls make samples
    // slower than single-threaded JS.
    timeout: Timeout.value(1000),

    codePrefix: """
        """,

    codeSuffix: """
        gc();
        """,

    ecmaVersion: ECMAScriptVersion.es6,

    startupTests: [
        // Check that the fuzzilli integration is available.
        ("fuzzilli('FUZZILLI_PRINT', 'test')", .shouldSucceed),

        // Check that common crash types are detected.
        ("fuzzilli('FUZZILLI_CRASH', 0)", .shouldCrash),
        ("fuzzilli('FUZZILLI_CRASH', 1)", .shouldCrash),
        // No FUZZILLI_CRASH 2 check: it is ASSERT(0), a no-op in the
        // RelWithDebInfo+ASAN fuzz build (only fires in Debug builds).

        // Check that the Thread API is actually exposed (--useJSThreads).
        (
            "if (typeof Thread !== 'function' || typeof Lock !== 'function' || typeof Condition !== 'function' || typeof ThreadLocal !== 'function') throw 'no Thread API'",
            .shouldSucceed
        ),

        // Check that a trivial spawn/join round-trips a value.
        ("if (new Thread(() => 42).join() !== 42) throw 'bad join'", .shouldSucceed),
    ],

    additionalCodeGenerators: [
        (threadsForceDFGCompilationGenerator, 5),
        (threadsForceFTLCompilationGenerator, 5),
        (threadsGcGenerator, 5),

        (threadSpawnGenerator, 15),
        (threadJoinGenerator, 5),
        (sharedObjectPropertyStormGenerator, 15),
        (sharedArrayResizeRaceGenerator, 10),
        (dictionaryFlipGenerator, 10),
        (threadRestrictGenerator, 10),
        (propertyAtomicsGenerator, 15),
        (propertyAtomicsWaitNotifyGenerator, 8),
        (lockContentionGenerator, 10),
        (conditionWaitNotifyGenerator, 5),
        (threadLocalGenerator, 5),
        (sharedProxyGetterGenerator, 8),
        (crossThreadJITWarmupGenerator, 10),
    ],

    additionalProgramTemplates: WeightedList<ProgramTemplate>([]),

    disabledCodeGenerators: [],

    disabledMutators: [],

    additionalBuiltins: [
        "gc": .function([] => .undefined),
        "transferArrayBuffer": .function([.object(ofGroup: "ArrayBuffer")] => .undefined),
        "noInline": .function([.function()] => .undefined),
        "noFTL": .function([.function()] => .undefined),
        "createGlobalObject": .function([] => .object()),
        "OSRExit": .function([] => .jsAnything),
        "drainMicrotasks": .function([] => .jsAnything),
        "runString": .function([.string] => .jsAnything),
        "makeMasquerader": .function([] => .jsAnything),
        "fullGC": .function([] => .undefined),
        "edenGC": .function([] => .undefined),
        "fiatInt52": .function([.number] => .number),
        "forceGCSlowPaths": .function([] => .jsAnything),
        "ensureArrayStorage": .function([] => .jsAnything),

        // Shared-memory Thread API (--useJSThreads).
        "Thread": jsThreadConstructorType,
        "Lock": jsLockConstructorType,
        "Condition": jsConditionConstructorType,
        "ThreadLocal": jsThreadLocalConstructorType,
        "ConcurrentAccessError": .constructor([.opt(.string)] => .object()),
    ],

    additionalObjectGroups: [
        jsThreadGroup, jsThreadConstructorGroup, jsLockGroup, jsConditionGroup,
        jsThreadLocalGroup,
    ],

    additionalEnumerations: [],

    additionalOptionsBags: [],

    optionalPostProcessor: nil
)
