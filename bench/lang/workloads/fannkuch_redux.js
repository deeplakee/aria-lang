// fannkuch_redux 参照端口(与 fannkuch_redux.aria 同算法同规模)。
const n = 9;

const t0 = performance.now();
const perm = [];
for (let i = 0; i < n; i++) {
    perm.push(i);
}
let maxFlips = 0;
let flipsSum = 0;
let permCount = 0;
for (;;) {
    if (perm[0] !== 0) {
        const work = perm.slice();
        let flips = 0;
        while (work[0] !== 0) {
            const k = work[0] + 1;
            let left = 0;
            let right = k - 1;
            while (left < right) {
                const tmp = work[left];
                work[left] = work[right];
                work[right] = tmp;
                left += 1;
                right -= 1;
            }
            flips += 1;
        }
        if (flips > maxFlips) {
            maxFlips = flips;
        }
        flipsSum += flips;
        permCount += 1;
    }
    let i = n - 2;
    while (i >= 0 && perm[i] >= perm[i + 1]) {
        i -= 1;
    }
    if (i < 0) {
        break;
    }
    let j = n - 1;
    while (perm[j] <= perm[i]) {
        j -= 1;
    }
    const tmp2 = perm[i];
    perm[i] = perm[j];
    perm[j] = tmp2;
    let left2 = i + 1;
    let right2 = n - 1;
    while (left2 < right2) {
        const tmp3 = perm[left2];
        perm[left2] = perm[right2];
        perm[right2] = tmp3;
        left2 += 1;
        right2 -= 1;
    }
}
const dt = performance.now() - t0;

check("fannkuch_redux.flips_sum", flipsSum, 1911505);
check("fannkuch_redux.max_flips", maxFlips, 30);
check("fannkuch_redux.perm_count", permCount, 322560);

console.log("value checksum: " + flipsSum);
console.log("value max_flips: " + maxFlips);
console.log("value perm_count: " + permCount);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
