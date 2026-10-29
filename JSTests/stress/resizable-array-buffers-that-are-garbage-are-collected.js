//@ slow!

// A resizable ArrayBuffer reserves its maximum. Past one page of 64 KiB that takes two mappings, and a Linux process has
// 65,530 of them by default. The size that the collector knows of such a buffer is its length, so a loop that makes
// small buffers with a large maximum could take every mapping before a collection ran. The heap asks for a full
// collection each time it has wrapped 4,096 buffers with a maximum past one page. The loops cannot be shorter: it
// takes 32,766 such buffers to use every mapping.

const maxByteLength = 1024 * 1024;

for (let i = 0; i < 150000; i++)
    new ArrayBuffer(16, { maxByteLength });

// A transfer of a pinned buffer makes a copy with the same maximum.
const pinned = new ArrayBuffer(16, { maxByteLength });
$vm.pinArrayBuffer(pinned);
for (let i = 0; i < 150000; i++)
    pinned.transfer();
$vm.unpinArrayBuffer(pinned);
