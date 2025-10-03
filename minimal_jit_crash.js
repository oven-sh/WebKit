// Minimal test to trigger JIT compilation crash
function hotFunction(n) {
    var sum = 0;
    for (var i = 0; i < n; i++) {
        sum += i * 2;
    }
    return sum;
}

// Trigger JIT by calling many times with different values
for (var j = 0; j < 15000; j++) {
    hotFunction(100);
}

print("Finished without crash");