(function() {
    var subjects = ["!'()*", "my-file(1)-copy.jpg", "a-b-c-d-e-f-g-h-i-j"];
    var replacer = function(c) { return "%" + c.charCodeAt(0).toString(16).toUpperCase(); };
    var result;
    for (var i = 0; i < 400000; ++i)
        result = subjects[i % 3].replace(/[!'()*-]/g, replacer);
    if (result != "%21%27%28%29%2A")
        throw "Error: bad result: " + result;
})();
