//@ runDefault("--useDollarVM=1", "--useJIT=0")

// As module-load-cut-short-by-termination.js, but the load that is cut runs inside the fetch hook of another load,
// and the termination is withdrawn there. The outer load goes on behind the hook: it has begun to load the same
// module, so some of its steps wait for promises of the load that was cut. Each of them asks again
// (JSModuleLoader::askAgainForAbandonedLoad()), and the outer load gets its modules.
//
// The modules are made by the fetch hook of the shell (setModuleLoaderFetchHook()). In each shape the hook for one
// module loads another module synchronously, with a termination at the n-th trap check, and the hook for a module that
// this load needs throws while the load runs, so the load never ends by itself inside the hook.

const directory = "./module-load-cut-short-by-termination-nested/";
const terminationValue = "JavaScript execution terminated.";
globalThis.bodies = new Map;

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(`${what}: expected ${expected}, got ${actual}`);
}

const module = (name, imports, value) => imports.map(([file, binding]) => `import { value as ${binding} } from "./${file}";\n`).join("")
    + `globalThis.bodies.set("${name}", (globalThis.bodies.get("${name}") ?? 0) + 1);\nexport const value = ${value};\n`;
const shapes = [
    {
        // root.js imports d.js and x.js, and d.js imports spin.js. The hook for x.js loads d.js, which the outer load
        // has asked for already.
        name: "siblings",
        sources: {
            "root.js": module("root.js", [["d.js", "d"], ["x.js", "x"]], "d + x"),
            "d.js": module("d.js", [["spin.js", "spin"]], "spin + 1"),
            "x.js": module("x.js", [], "1"),
            "spin.js": module("spin.js", [], "39"),
        },
        hookFile: "x.js",
        loadedInHook: "d.js",
        endsLoadInHook: "spin.js",
        values: { "root.js": 41, "d.js": 40 },
    },
    {
        // root.js and m.js import each other. m.js imports w.js and k.js, k.js imports z.js, and w.js imports w2.js.
        // The hook for z.js loads w.js. A module of a cycle is in the list of loaded modules of its importer before
        // its own load has ended.
        name: "cycle",
        sources: {
            "root.js": module("root.js", [["m.js", "m"]], "m"),
            "m.js": `import "./root.js";\n` + module("m.js", [["w.js", "w"], ["k.js", "k"]], "w + k"),
            "k.js": module("k.js", [["z.js", "z"]], "z"),
            "w.js": module("w.js", [["w2.js", "w2"]], "w2 + 1"),
            "z.js": module("z.js", [], "1"),
            "w2.js": module("w2.js", [], "39"),
        },
        hookFile: "z.js",
        loadedInHook: "w.js",
        endsLoadInHook: "w2.js",
        values: { "root.js": 41, "w.js": 40 },
    },
];
const endOfNestedLoad = new Error("the load inside the hook ends here");

let shape = shapes[0];
let round = null;
setModuleLoaderFetchHook(key => {
    const name = key.slice(key.lastIndexOf("/") + 1);
    if (!round || !(name in shape.sources))
        return shape.sources[name];
    if (name == shape.endsLoadInHook && round.inHook)
        throw endOfNestedLoad;
    if (name == shape.hookFile && !round.hookRan) {
        round.hookRan = true;
        round.inHook = true;
        try {
            round.wasCut = $vm.callWithTerminationAtTrapCheck(() => {
                $vm.moduleLoaderLoadSync(round.loader, directory + shape.loadedInHook);
            }, round.n);
        } catch (error) {
            round.threw = error;
        }
        round.inHook = false;
    }
    return shape.sources[name];
});

function importFile(loader, file) {
    const load = { file, state: "pending", value: undefined };
    $vm.moduleLoaderImport(loader, directory + file).then(namespace => {
        load.value = namespace.value;
        load.state = "fulfilled";
    }, error => {
        load.value = error;
        load.state = "rejected";
    });
    return load;
}

// A load has its module, or the error with which the load inside the hook ended by itself before the termination came.
// It does not wait, and it is not answered with anything else.
function check(what, load) {
    if (load.state == "pending")
        throw new Error(`${what}: ${load.file} is still pending`);
    if (load.state == "rejected") {
        shouldBe(load.value, endOfNestedLoad, `${what}: what ${load.file} rejected with`);
        return false;
    }
    shouldBe(load.value, shape.values[load.file], `${what}: the value of ${load.file}`);
    return true;
}

const outerLoads = {
    // The outer load is an import() and its checkpoint.
    "import()": loader => {
        const root = importFile(loader, "root.js");
        drainMicrotasks();
        return root;
    },
    // The outer load is a synchronous one.
    "synchronous load": loader => {
        const root = { file: "root.js", state: "fulfilled", value: undefined };
        try {
            root.value = $vm.moduleLoaderLoadSync(loader, directory + "root.js").value;
        } catch (error) {
            root.state = "rejected";
            root.value = error;
        }
        return root;
    },
};

for (shape of shapes) {
for (const [outerName, outerLoad] of Object.entries(outerLoads)) {
    // The trap checks of a function are at fixed places once its inline caches are full.
    for (let i = 0; i < 10; ++i)
        outerLoad($vm.createModuleLoader());

    let roundsWithModule = 0;
    for (let n = 1; ; ++n) {
        if (n > 100000)
            throw new Error(`${shape.name}, ${outerName}: the load inside the hook has no end`);
        const what = `${shape.name}, ${outerName}, trap check ${n} of the load inside the hook`;
        const loader = $vm.createModuleLoader();
        bodies.clear();
        round = { loader, n, hookRan: false, inHook: false, wasCut: undefined, threw: undefined };
        const outer = outerLoad(loader);
        const { wasCut, threw } = round;
        round = null;
        shouldBe($vm.hasPendingTermination(), false, `${what}: a termination is still pending`);

        const laterRoot = importFile(loader, "root.js");
        drainMicrotasks();
        const laterLoadedInHook = importFile(loader, shape.loadedInHook);
        drainMicrotasks();
        const outerHasModule = check(`${what}: the outer load`, outer);
        shouldBe(check(`${what}: a later import()`, laterRoot), outerHasModule, `${what}: a later import() of root.js has the module`);
        check(`${what}: a later import()`, laterLoadedInHook);
        for (const [file, runs] of bodies) {
            if (runs > 1)
                throw new Error(`${what}: the body of ${file} ran ${runs} times`);
        }
        if (outerHasModule)
            shouldBe(bodies.size, Object.keys(shape.sources).length, `${what}: the modules that have run`);

        if (!wasCut) {
            // The load inside the hook ended with the error of the hook for the module that it needs.
            shouldBe(threw, endOfNestedLoad, `${what}: what the load inside the hook threw`);
            break;
        }
        if (outerHasModule)
            ++roundsWithModule;
    }
    if (!roundsWithModule)
        throw new Error(`${shape.name}, ${outerName}: no outer load got its module behind a cut`);
}
}
