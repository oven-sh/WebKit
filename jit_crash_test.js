// Test to trigger JIT compilation and potential crash
function hotFunction(x) {
    let result = 0;
    for (let i = 0; i < 1000; i++) {
        result += x * i + Math.sin(i) + Math.cos(x);
    }
    return result;
}

// Call it many times to trigger JIT compilation
print("Starting JIT crash test...");
for (let i = 0; i < 10000; i++) {
    if (i % 1000 === 0) {
        print("Iteration " + i);
    }
    hotFunction(i);
}
print("Test completed successfully");