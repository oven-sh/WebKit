function eq(a, b, what) { if (a !== b && !(a !== a && b !== b)) throw new Error(`${what}: expected ${b}, got ${a}`); }
function near(a, b, what) { if (Math.abs(a - b) > 1e-9 * Math.max(1, Math.abs(b))) throw new Error(`${what}: expected ${b}, got ${a}`); }
const loadC = name => $vm.cModule(readFile(`./out/${name}.bir`, "binary"));
const cstr = s => { const b = new Uint8Array(s.length + 1); for (let i = 0; i < s.length; i++) b[i] = s.charCodeAt(i); return b; };
const fromC = b => { let s = ""; for (let i = 0; b[i]; i++) s += String.fromCharCode(b[i]); return s; };

{
    const m = loadC("fib");
    eq(m.fib(20), 6765, "fib");
    eq(m.fib_iter(70), 190392490709135n, "fib_iter"); eq(m.fib_iter(80), 23416728348467685n, "fib_iter big");
    eq(m.gcd(1071, 462), 21, "gcd");
    eq(m.gcd(4000000000, 3000000000), 1000000000, "gcd unsigned");
    eq(m.collatz_steps(27), 111, "collatz");
    eq(m.count_primes(10000), 1229, "primes");
    eq(m.count_primes(100), 25, "primes again (static array reset)");
}
{
    const m = loadC("struct");
    eq(m.rect_size(), 40, "sizeof Rect");
    near(m.scaled_unit_area(3), 9 + 7, "scaled_unit_area");
    const buf = new Float64Array(5);
    m.make_rect(buf, 4, 2.5);
    eq(buf[0], 0, "min.x"); eq(buf[2], 4, "max.x"); eq(buf[3], 2.5, "max.y");
    eq(new Int32Array(buf.buffer)[8], 1, "tag");
    near(m.rect_area(buf), 10, "rect_area");
    m.rect_scale(buf, 2);
    near(m.rect_area(buf), 40, "rect_area scaled");
    eq(m.list_sum_of_squares(10), 385, "list");
    eq(m.list_sum_of_squares(100), 1496, "list capped at 16");
}
{
    const m = loadC("strings");
    eq(m.my_strlen(cstr("abcdef")), 6, "my_strlen");
    eq(m.hello_length(), 12, "literal concat");
    eq($vm.ffiCString(m.weekday(9)), "Wed", "weekday reloc table");
    const fnv = s => { let h = 2166136261; for (const c of s) { h ^= c.charCodeAt(0); h = Math.imul(h, 16777619) >>> 0; } return h >>> 0; };
    eq(m.fnv1a(cstr("hello"), 5), fnv("hello"), "fnv1a");
    eq(m.hash_weekday(4), fnv("Fri"), "hash_weekday");
    const s = cstr("Hello, World 42");
    eq(m.to_upper(s), 8, "to_upper count"); eq(fromC(s), "HELLO, WORLD 42", "to_upper");
    const out = new Uint8Array(16);
    eq(m.format_int(-2147483648, out), 11, "format_int len"); eq(fromC(out), "-2147483648", "format_int min");
    eq(m.format_int(0, out), 1, "format_int 0"); eq(fromC(out), "0", "format_int zero");
}
{
    const m = loadC("fnptr");
    eq(m.apply(0, 7, 5), 12, "add"); eq(m.apply(1, 7, 5), 2, "sub"); eq(m.apply(2, 7, 5), 35, "mul"); eq(m.apply(3, 7, 0), 0, "div0"); eq(m.apply(3, -7, 2), -3, "div");
    eq(m.apply_by_letter("m".charCodeAt(0), 6, 7), 42, "by letter"); eq(m.apply_by_letter("x".charCodeAt(0), 6, 7), -1, "by letter miss");
    eq(m.sum_and_product(), 15120, "fold");
    eq(m.classify(32), 0, "space"); eq(m.classify(55), 1, "digit"); eq(m.classify(95), 2, "_"); eq(m.classify(113), 2, "q"); eq(m.classify(64), 3, "@");
    // native callback: pass one C function's pointer to another module's function
    const fibm = loadC("fib");
    eq(m.call_twice(fibm.fib.ptr, 7), 233, "call_twice(fib, 7) = fib(13)");
    // JS callback
    const cb = $vm.ffiCallback({ args: ["i32"], returns: "i32" }, x => x * 3);
    eq(m.call_twice(cb.ptr, 5), 45, "call_twice(js)");
}
{
    const m = loadC("floats");
    near(m.lerp(10, 20, 0.25), 12.5, "lerp");
    near(m.average3(1, 2, 4), Math.fround(7 / 3), "average3");
    near(m.my_sqrt(2), Math.SQRT2, "sqrt");
    eq(m.round_to_int(2.5), 3, "round"); eq(m.round_to_int(-2.5), -3, "round neg");
    near(m.int_ratio(1, 3), 1 / 3, "ratio");
    eq(m.to_unsigned(4000000000.7), 4000000000, "to_unsigned");
    eq(m.from_unsigned(4000000000), 4000000000, "from_unsigned");
    eq(m.from_u64(2 ** 53), 2 ** 53, "from_u64");
    eq(m.narrow(0.1), Math.fround(0.1), "narrow");
    near(m.mat_trace_of_square(1, 2, 3, 4), 7 + 22, "trace");
}
{
    const m = loadC("printf");
    eq(m.greet(cstr("bun")), "hello, bun! (3 chars)\n".length, "greet");
    m.print_table(3);
    eq(m.print_wide(1234567890123, 2.5), 1234567890123n, "print_wide");
}
print("e2e ok");
