let keep = [];
for (let it = 0; it < 30; it++) {
  const batch = new Array(20000);
  for (let i = 0; i < batch.length; i++) { const arr = new Array(16); for (let j = 0; j < 16; j++) arr[j] = { a: i, b: j }; batch[i] = arr; }
  keep.push(batch); if (keep.length > 3) keep.shift();
  let head = null; for (let i = 0; i < 50000; i++) head = { next: head, i }; keep[0].chain = head;
}
print("done", keep.length);
