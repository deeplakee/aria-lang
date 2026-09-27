// bank_queue 参照端口(与 bank_queue.aria 同算法同规模)。
const customers_n = 78000;
const key_unit = 1000000;

let state = 424242;
function rnd(n) {
    state = (state * 48271) % 2147483647;
    return state % n;
}

class Teller {
    constructor() {
        this.busy_until = 0;
        this.served = 0;
    }
}

const tellers = [];
let ti = 0;
while (ti < 4) {
    tellers.push(new Teller());
    ti += 1;
}

const heap = [];
const queue = [];
let seq = 0;
let served = 0;
let wait_sum = 0;
let last_time = 0;

function heap_push(entry) {
    heap.push(entry);
    let i = heap.length - 1;
    while (i > 0) {
        const parent = Math.floor((i - 1) / 2);
        if (heap[parent][0] <= heap[i][0]) {
            break;
        }
        const tmp = heap[parent];
        heap[parent] = heap[i];
        heap[i] = tmp;
        i = parent;
    }
}

function heap_pop() {
    const top = heap[0];
    const last = heap.pop();
    if (heap.length > 0) {
        heap[0] = last;
        let i = 0;
        const n = heap.length;
        while (true) {
            const l = 2 * i + 1;
            const r = l + 1;
            let m = i;
            if (l < n && heap[l][0] < heap[m][0]) {
                m = l;
            }
            if (r < n && heap[r][0] < heap[m][0]) {
                m = r;
            }
            if (m === i) {
                break;
            }
            const tmp = heap[m];
            heap[m] = heap[i];
            heap[i] = tmp;
            i = m;
        }
    }
    return top;
}

function start_service(idx, now) {
    served += 1;
    tellers[idx].served += 1;
    const svc = 20 + rnd(60);
    tellers[idx].busy_until = now + svc;
    seq += 1;
    heap_push([(now + svc) * key_unit + seq, 1, idx]);
}

function on_arrival(key) {
    const now = Math.floor(key / key_unit);
    last_time = now;
    const next = now + 1 + rnd(40);
    seq += 1;
    heap_push([next * key_unit + seq, 0, 0]);
    let free = -1;
    let k = 0;
    while (k < tellers.length) {
        if (tellers[k].busy_until <= now) {
            free = k;
        }
        k += 1;
    }
    if (free >= 0) {
        start_service(free, now);
    } else {
        queue.push(now);
    }
}

function on_done(key, idx) {
    const now = Math.floor(key / key_unit);
    last_time = now;
    if (queue.length > 0) {
        const arrive = queue.shift();
        wait_sum += now - arrive;
        start_service(idx, now);
    }
}

const t0 = performance.now();

seq += 1;
heap_push([(1 + rnd(40)) * key_unit + seq, 0, 0]);
while (served < customers_n) {
    const ev = heap_pop();
    if (ev[1] === 0) {
        on_arrival(ev[0]);
    } else {
        on_done(ev[0], ev[2]);
    }
}

let load_sum = 0;
let k2 = 0;
while (k2 < tellers.length) {
    load_sum += tellers[k2].served;
    k2 += 1;
}

check("served", served, customers_n);
check("load_sum", load_sum, served);
check("wait_sum", wait_sum, 101322);
check("last_time", last_time, 1604597);

const dt = performance.now() - t0;

console.log("value served: " + served);
console.log("value wait_sum: " + wait_sum);
console.log("value last_time: " + last_time);
console.log("value load_sum: " + load_sum);
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
