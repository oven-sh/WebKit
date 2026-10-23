//@ requireOptions("--airGreedyRegAllocSplitAroundLoops=true")

// A value that is live over a nest of loops is split around the outer loop, and the outer loop's part once more around an inner loop.

function f(v0, v1, v2, v3, v4, v5, v6, v7, v8, v9, v10, v11, n) {
  let s = 0;
  s = v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + v10 + v11 | 0;
  for (let i0 = 0; i0 < n; i0++) {
    s = (s + v0) ^ i0;
    s = (s + v1) ^ i0;
    s = (s + v2) ^ i0;
    s = (s + v3) ^ i0;
    s = (s + v4) ^ i0;
    s = (s + v5) ^ i0;
    s = (s + v6) ^ i0;
    s = (s + v7) ^ i0;
    s = (s + v8) ^ i0;
    s = (s + v9) ^ i0;
    s = (s + v10) ^ i0;
    s = (s + v11) ^ i0;
    for (let i1 = 0; i1 < n; i1++) {
      s = (s + v0) ^ i1;
      s = (s + v1) ^ i1;
      s = (s + v2) ^ i1;
      s = (s + v3) ^ i1;
      s = (s + v4) ^ i1;
      s = (s + v5) ^ i1;
      s = (s + v6) ^ i1;
      s = (s + v7) ^ i1;
      s = (s + v8) ^ i1;
      s = (s + v9) ^ i1;
      s = (s + v10) ^ i1;
      s = (s + v11) ^ i1;
      for (let i2 = 0; i2 < n; i2++) {
        s = (s + v0) ^ i2;
        s = (s + v1) ^ i2;
        s = (s + v2) ^ i2;
        s = (s + v3) ^ i2;
        s = (s + v4) ^ i2;
        s = (s + v5) ^ i2;
        s = (s + v6) ^ i2;
        s = (s + v7) ^ i2;
        s = (s + v8) ^ i2;
        s = (s + v9) ^ i2;
        s = (s + v10) ^ i2;
        s = (s + v11) ^ i2;
        s = (s - v1) ^ i2;
        s = (s - v3) ^ i2;
        s = (s - v5) ^ i2;
        s = (s - v7) ^ i2;
        s = (s - v9) ^ i2;
        s = (s - v11) ^ i2;
      }
      s = (s - v1) ^ i1;
      s = (s - v3) ^ i1;
      s = (s - v5) ^ i1;
      s = (s - v7) ^ i1;
      s = (s - v9) ^ i1;
      s = (s - v11) ^ i1;
    }
    s = (s - v1) ^ i0;
    s = (s - v3) ^ i0;
    s = (s - v5) ^ i0;
    s = (s - v7) ^ i0;
    s = (s - v9) ^ i0;
    s = (s - v11) ^ i0;
  }
  return (s + v0 + v1 + v2 + v3 + v4 + v5 + v6 + v7 + v8 + v9 + v10 + v11) | 0;
}
let total = 0;
for (let r = 0; r < 4000; r++)
  total = (total + f(r + 0, r + 1, r + 2, r + 3, r + 4, r + 5, r + 6, r + 7, r + 8, r + 9, r + 10, r + 11, 3)) | 0;
if (total !== 2068544000)
    throw new Error("bad result: " + total);
