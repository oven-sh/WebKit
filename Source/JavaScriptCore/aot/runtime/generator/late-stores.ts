import { mkdirSync, readdirSync, readFileSync, writeFileSync } from "node:fs";
import { join, resolve } from "node:path";
import { parseArgs } from "node:util";

const listFile = "aot/runtime/AOTLateStores.txt";
const stampFile = "AOTLateStoresChecked.h";
const foldersOfTheEngine = ["builtins", "object", "runtime"];
const ownFileOfTheBindings = /^Bun(?:AOT|Compiled|WithoutAOT)\w*\.(?:cpp|h)$/;
const prefixOfTheBindings = "bindings/";

const lateStore = "late store";
const behindFinishCreation = "behind finishCreation";

const storesWithoutBarrier =
  /\bsetWithoutWriteBarrier\s*\(|WithoutBarrier\s*\(|\bstoreWordOf\w+\s*\(|\batUnsafe\([^)]*\)\s*=[^=]|\btryWriteSlot\s*\(|\bstoreBitsOfWidth\s*\(|\*\s*reinterpret_cast<[^;=]*\*>\s*\([^;]*\)\s*=[^=]/;
const asksWhetherTryFailed = /!\s*tryWriteSlot\s*\(/;
const copies = /\b(?:std::copy_n|std::ranges::copy|memcpy|memcpySpan|gcSafeMemcpy|gcSafeMemmove)\s*\(/;
const namesWordsThatMayBeCells = /indexingPayload|butterfly|propertyStorage|inlineStorage|WriteBarrier|JSValue|uint64_t/;
const ordersOrRemembers =
  /\bmutatorFence\b|\bstoreStoreFence\b|\bstoreLoadFence\b|\bwriteBarrier\b|\bfinishCreation\b|\bcreateWithButterfly|\.set\(vm\b|\bsetIndex\(vm\b|\bputDirect|\binitializeIndex\b|\bdidStoreRootInStatics\b/;
const leaves = /\breturn\b|\bOPERATION_RETURN\(|\bRELEASE_AND_RETURN\b/;
const storesIntoOwnField = /\bsetWithoutWriteBarrier\s*\(|^\s*m_\w+(?:\[[^\]]*\])?\s*=[^=]/;
const ordersBehindFinishCreation = /\.set\(|\bwriteBarrier\b|\bmutatorFence\b/;

const whatToWrite: Record<string, string> = {
  [lateStore]: "Behind the last store that fills a new object stands vm.mutatorFence(), or the object's barrier",
  [behindFinishCreation]: "Base::finishCreation() has the fence of a new object: what fills the object stands in front of it, or has .set(), a barrier or vm.mutatorFence() behind it",
};

type Counts = Map<string, number>;

function keyOf(net: string, file: string, name: string): string {
  return `${net} | ${file} | ${name}`;
}

function nameOfFunction(lines: string[], openingBrace: number): string {
  for (let index = openingBrace - 1; index >= 0 && index >= openingBrace - 14; --index) {
    const line = lines[index]!;
    if (!line || /^[\s,:#/}]/.test(line)) continue;
    return (/^JSC_DEFINE\w*\((\w+)/.exec(line) ?? /([\w:~]+)\s*\(/.exec(line))?.[1] ?? line.slice(0, 50);
  }
  return "?";
}

function countIn(text: string, file: string, counts: Counts): void {
  const lines = text.split("\n");
  const add = (net: string, name: string) => counts.set(keyOf(net, file, name), (counts.get(keyOf(net, file, name)) ?? 0) + 1);
  let openingBrace = -1;
  for (let index = 0; index < lines.length; ++index) {
    if (lines[index] === "{" && openingBrace < 0) openingBrace = index;
    if (lines[index] !== "}" || openingBrace < 0) continue;
    const body = lines.slice(openingBrace, index + 1).map(line => (line.trimStart().startsWith("//") ? "" : line));
    const name = nameOfFunction(lines, openingBrace);
    openingBrace = -1;

    if (!body.some(line => line.includes("ObjectInitializationScope"))) {
      body.forEach((line, at) => {
        if (!storesWithoutBarrier.test(line) && !(copies.test(line) && namesWordsThatMayBeCells.test(line))) return;
        const first = asksWhetherTryFailed.test(line) && leaves.test(body[at + 1] ?? "") ? at + 1 : at;
        const next = body.findIndex((later, where) => where > first && leaves.test(later));
        const end = next < 0 ? body.length - 1 : next;
        if (!body.slice(at + 1, end + 1).some(later => ordersOrRemembers.test(later))) add(lateStore, name);
      });
    }

    if (!/(?:^|::)finishCreation$/.test(name)) continue;
    const base = body.findIndex(line => /\bBase::finishCreation\s*\(/.test(line));
    if (base < 0) continue;
    const last = body.findLastIndex((line, at) => at > base && storesIntoOwnField.test(line));
    if (last > base && !body.slice(last + 1).some(line => ordersBehindFinishCreation.test(line))) add(behindFinishCreation, name);
  }
}

function sourcesBelow(directory: string, prefix: string): string[] {
  return readdirSync(directory, { withFileTypes: true })
    .sort((a, b) => (a.name < b.name ? -1 : 1))
    .flatMap(entry => {
      if (entry.isDirectory()) return sourcesBelow(join(directory, entry.name), `${prefix}${entry.name}/`);
      return entry.isFile() && /\.(?:cpp|h)$/.test(entry.name) ? [`${prefix}${entry.name}`] : [];
    });
}

function countPlaces(javascriptcore: string, bindings: string | undefined): Counts {
  const counts: Counts = new Map();
  for (const folder of foldersOfTheEngine) {
    for (const file of sourcesBelow(join(javascriptcore, "aot", folder), `${folder}/`)) countIn(readFileSync(join(javascriptcore, "aot", file), "utf8"), file, counts);
  }
  if (bindings === undefined) return counts;
  for (const entry of readdirSync(bindings).sort()) {
    if (ownFileOfTheBindings.test(entry)) countIn(readFileSync(join(bindings, entry), "utf8"), `${prefixOfTheBindings}${entry}`, counts);
  }
  return counts;
}

function readList(text: string, withBindings: boolean): { listed: Counts; problems: string[] } {
  const listed: Counts = new Map();
  const problems: string[] = [];
  for (const line of text.split("\n")) {
    if (!line || line.startsWith("#")) continue;
    const [net, file, name, count, reason] = line.split(" | ");
    if (!net || !(net in whatToWrite) || !file || !name || !/^\d+$/.test(count ?? "") || !reason?.trim())
      problems.push(`${listFile}: not "<${lateStore} or ${behindFinishCreation}> | <file> | <function> | <how often> | <why each is right>": ${line}`);
    else if (withBindings || !file.startsWith(prefixOfTheBindings)) listed.set(keyOf(net, file, name), Number(count));
  }
  return { listed, problems };
}

function compare(found: Counts, listed: Counts): string[] {
  const problems: string[] = [];
  for (const [key, count] of found) {
    const allowed = listed.get(key) ?? 0;
    if (count === allowed) continue;
    const [net, file, name] = key.split(" | ");
    if (count > allowed)
      problems.push(`${file}: ${name}() has ${count} of "${net}", the list has ${allowed}. ${whatToWrite[net!]}; or say in ${listFile} why this one is right as it is.`);
    else problems.push(`${file}: ${name}() has ${count} of "${net}", the list has ${allowed}: lower the count in ${listFile}, or the next one passes unseen.`);
  }
  for (const [key, allowed] of listed) {
    const [net, file, name] = key.split(" | ");
    if (!found.has(key)) problems.push(`${file}: ${name}() has none of "${net}", the list has ${allowed}: take the line out of ${listFile}.`);
  }
  return problems;
}

function hasTeeth(found: Counts): boolean {
  const countOf = (body: string[], head = "static JSArray* make(VM& vm)") => {
    const counts: Counts = new Map();
    countIn([head, "{", ...body.map(line => `    ${line}`), "}", ""].join("\n"), "file.cpp", counts);
    return [...counts.values()].reduce((sum, count) => sum + count, 0);
  };
  const store = "elements.atUnsafe(i).setWithoutWriteBarrier(value);";
  const cases: [number, number][] = [
    [countOf([store, "return array;"]), 1],
    [countOf([store, "vm.mutatorFence();", "return array;"]), 0],
    [countOf([store, "vm.writeBarrier(array);", "return array;"]), 0],
    [countOf(["vm.mutatorFence();", store, "return array;"]), 1],
    [countOf([`// ${store}`, "return array;"]), 0],
    [countOf([store, store, "return array;"]), 2],
    [countOf(["*reinterpret_cast<EncodedJSValue*>(words + offset) = word;", "return array;"]), 1],
    [countOf(["uint64_t word = *reinterpret_cast<const uint64_t*>(words + offset);", "return array;"]), 0],
    [countOf(["storeBitsOfWidth(width, slot, bits);", "return array;"]), 1],
    [countOf(["if (!tryWriteSlot(instance, representation, slot, value))", "    return nullptr;", "return array;"]), 1],
    [countOf(["if (!tryWriteSlot(instance, representation, slot, value))", "    return nullptr;", "vm.writeBarrier(array);", "return array;"]), 0],
    [countOf(["tryWriteSlot(instance, representation, slot, value);", "statics.didStoreRootInStatics(offset);", "return array;"]), 0],
    [countOf([store, "// vm.mutatorFence();", "return array;"]), 1],
    [countOf([store, "// return early;", "vm.mutatorFence();", "return array;"]), 0],
    [countOf(["// ObjectInitializationScope scope(vm);", store, "return array;"]), 1],
    [countOf(["if (small) {", store, "return array;", "}", store, "return JSArray::createWithButterfly(vm, nullptr, structure, butterfly);"]), 1],
    [countOf(["ObjectInitializationScope scope(vm);", store, "return array;"]), 0],
    [countOf(["std::copy_n(from->butterfly()->indexingPayload<uint64_t>(), count, to);", "return array;"]), 1],
    [countOf(["memcpy(characters, source, length);", "return array;"]), 0],
    [countOf(["storeWordOfState(state, Word::Next, next);", "OPERATION_RETURN(scope, state);"], "JSC_DEFINE_JIT_OPERATION(operationOpen, JSCell*, (Instance* instance))"), 1],
    [countOf(["Base::finishCreation(vm);", "m_callback = callback;"], "void Holder::finishCreation(VM& vm, JSCell* callback)"), 1],
    [countOf(["Base::finishCreation(vm);", "m_callback = callback;", "vm.mutatorFence();"], "void Holder::finishCreation(VM& vm, JSCell* callback)"), 0],
    [countOf(["m_callback = callback;", "Base::finishCreation(vm);"], "void Holder::finishCreation(VM& vm, JSCell* callback)"), 0],
    [countOf(["Base::finishCreation(vm);", "m_callback.set(vm, this, callback);"], "void Holder::finishCreation(VM& vm, JSCell* callback)"), 0],
  ];
  if (!cases.every(([counted, expected]) => counted === expected)) return false;
  const [key, count] = [...found][0] ?? [];
  if (key === undefined || count === undefined) return false;
  const more = new Map(found).set(key, count + 1);
  const fewer = new Map(found).set(key, count - 1);
  const elsewhere = new Map(found).set(keyOf(lateStore, "runtime/AOTNoSuchFile.cpp", "noSuchFunction"), 1);
  return !compare(found, found).length && [more, fewer, elsewhere].every(changed => compare(changed, found).length === 1 && compare(found, changed).length === 1);
}

const { values } = parseArgs({
  strict: true,
  options: { "javascriptcore": { type: "string" }, "bindings": { type: "string" }, "out": { type: "string" }, "print-counts": { type: "boolean" } },
});
if (values.javascriptcore === undefined || (values.out === undefined && !values["print-counts"])) {
  console.error("usage: <bun or node> aot/runtime/generator/late-stores.ts --javascriptcore <Source/JavaScriptCore> [--bindings <src/jsc/bindings of Bun>] (--out <dir> | --print-counts)");
  process.exit(1);
}
const javascriptcore = resolve(values.javascriptcore);
const found = countPlaces(javascriptcore, values.bindings === undefined ? undefined : resolve(values.bindings));
if (values["print-counts"]) {
  for (const [key, count] of found) console.log(`${key} | ${count}`);
  process.exit(0);
}
const { listed, problems } = readList(readFileSync(join(javascriptcore, listFile), "utf8"), values.bindings !== undefined);
if (!hasTeeth(found)) problems.push("aot/runtime/generator/late-stores.ts does not notice a store too many or too few: it checks nothing.");
problems.push(...compare(found, listed));
if (problems.length) {
  console.error(problems.join("\n"));
  process.exit(1);
}
mkdirSync(values.out!, { recursive: true });
writeFileSync(join(values.out!, stampFile), "// Written by aot/runtime/generator/late-stores.ts when the sources of the runtime have passed it. Do not edit.\n");
