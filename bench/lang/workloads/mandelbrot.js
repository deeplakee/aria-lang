// mandelbrot 参照端口(与 mandelbrot.aria 同算法同规模)。
const size = 500;
const maxIter = 50;

const t0 = performance.now();
let checksum = 0;
let inside = 0;
for (let pyIdx = 0; pyIdx < size; pyIdx++) {
    const y0 = (pyIdx * 1.0 / size) * 2.0 - 1.0;
    for (let px = 0; px < size; px++) {
        const x0 = (px * 1.0 / size) * 2.5 - 2.0;
        let x = 0.0;
        let y = 0.0;
        let i = 0;
        while (i < maxIter) {
            const x2 = x * x;
            const y2 = y * y;
            if (x2 + y2 > 4.0) {
                break;
            }
            y = 2.0 * x * y + y0;
            x = x2 - y2 + x0;
            i += 1;
        }
        checksum = (checksum * 2 + i) % 1000000007;
        if (i === maxIter) {
            inside += 1;
        }
    }
}
const dt = performance.now() - t0;

check("mandelbrot.checksum", checksum, 140394512);
check("mandelbrot.inside", inside, 79596);
check("mandelbrot.pixels", size * size, 250000);

console.log("value checksum: " + checksum);
console.log("value inside: " + inside);
console.log("value pixels: " + size * size);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
