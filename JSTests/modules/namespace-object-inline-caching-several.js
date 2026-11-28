import { shouldBe } from "./resources/assert.js";
import * as m0 from "./namespace-object-inline-caching-several/m0.js"
import * as m1 from "./namespace-object-inline-caching-several/m1.js"
import * as m2 from "./namespace-object-inline-caching-several/m2.js"
import * as m3 from "./namespace-object-inline-caching-several/m3.js"
import * as m4 from "./namespace-object-inline-caching-several/m4.js"
import * as m5 from "./namespace-object-inline-caching-several/m5.js"
import * as m6 from "./namespace-object-inline-caching-several/m6.js"
import * as m7 from "./namespace-object-inline-caching-several/m7.js"
import * as m8 from "./namespace-object-inline-caching-several/m8.js"
import * as m9 from "./namespace-object-inline-caching-several/m9.js"

const namespaces = [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9];

function read(object)
{
    return object.value;
}
noInline(read);

// One site reads through more and more namespace objects, in the end more than an inline cache lists.
// This checks what a list of several namespace cases reads. It passes too when the list only ever holds one of them:
// the other reads are then slower, not different, and a script cannot see which it was.
for (let count = 1; count <= namespaces.length; ++count) {
    for (let i = 0; i < testLoopCount / 10; ++i) {
        for (let j = 0; j < count; ++j)
            shouldBe(read(namespaces[j]), j);
    }
}

// The cached reads see the bindings change.
for (let j = 0; j < namespaces.length; ++j)
    namespaces[j].set(j + 100);
for (let i = 0; i < testLoopCount / 10; ++i) {
    for (let j = 0; j < namespaces.length; ++j)
        shouldBe(read(namespaces[j]), j + 100);
}

// Ordinary objects share the site with them.
for (let i = 0; i < testLoopCount / 10; ++i) {
    shouldBe(read({ value: -1 }), -1);
    shouldBe(read(m3), 103);
    shouldBe(read({ other: 0, value: -2 }), -2);
    shouldBe(read(m9), 109);
}
