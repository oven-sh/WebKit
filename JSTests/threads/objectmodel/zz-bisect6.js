load("../harness.js", "caller relative");
function readAt(a, i) { return a[i]; }
const strings = [];
for (let i = 0; i < 10; ++i)
    strings.push("s" + i);
delete strings[5];
print(readAt(strings, 5));
print("ok");
