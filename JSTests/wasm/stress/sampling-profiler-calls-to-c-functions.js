import { instantiate } from "../gc/wast-wrapper.js";

// The top frame has to be known while WebAssembly code waits for a C function.
if (platformSupportsSamplingProfiler()) {
    const operations = {
        memoryFill: "(memory.fill (i32.const 0) (i32.const 1) (local.get $size))",
        memoryCopy: "(memory.copy (i32.const 1) (i32.const 0) (local.get $size))",
        memoryInit: "(memory.init $data (i32.const 0) (i32.const 0) (local.get $size))",
        tableFill: "(table.fill $table (i32.const 0) (ref.null extern) (local.get $size))",
        tableCopy: "(table.copy $table $table (i32.const 1) (i32.const 0) (local.get $size))",
        arrayFill: "(array.fill $array (global.get $first) (i32.const 0) (local.get $size) (local.get $size))",
        arrayCopy: "(array.copy $array $array (global.get $first) (i32.const 0) (global.get $second) (i32.const 0) (local.get $size))",
        arrayInitData: "(array.init_data $array $data (global.get $first) (i32.const 0) (i32.const 0) (i32.shr_u (local.get $size) (i32.const 2)))",
    };
    const size = 20000;
    const { exports } = instantiate(`(module
        (type $array (array (mut i32)))
        (memory 1)
        (table $table ${size + 1} externref)
        (data $data "${"a".repeat(size)}")
        (global $first (ref $array) (array.new_default $array (i32.const ${size})))
        (global $second (ref $array) (array.new_default $array (i32.const ${size})))
        ${Object.entries(operations).map(([name, operation]) => `
            (func (export "${name}") (param $count i32) (param $size i32)
                (loop $loop
                    ${operation}
                    (br_if $loop (local.tee $count (i32.sub (local.get $count) (i32.const 1))))))`).join("")})`);

    // The conversion is made by a function that tells the VM which frame called it, so that this is the last frame to be known.
    const object = { };
    const startTime = Date.now();
    for (const name of Object.keys(operations)) {
        const caller = new Function(`return function caller(callee, object, count, size) { String(object); callee(count, size); } // ${name}`)();
        noInline(caller);
        for (let i = 0; i < wasmTestLoopCount; i++)
            caller(exports[name], object, 2, 1);

        // A thread that is kept from running collects samples wherever it happens to be, so one batch proves nothing wrong.
        for (let inWasm = 0, total = 0; total < 40 || inWasm < 0.8 * total;) {
            if (Date.now() - startTime > 50000)
                throw new Error(`${name}: ${inWasm} of ${total}`);
            if (total >= 40)
                inWasm = total = 0;
            startSamplingProfiler({ intervalInMicroseconds: 100, recordsSamplesWithoutFrames: true });
            for (const end = preciseTime() + 0.002; preciseTime() < end;)
                caller(exports[name], object, 50, size);
            pauseSamplingProfiler();
            for (const { frames } of releaseSamplingProfilerSamples().stackTraces) {
                inWasm += frames[0].includes("wasm-function");
                total += frames.includes("caller");
            }
        }
    }
}
