// binary_trees 参照端口(与 binary_trees.aria 同算法同规模)。
const depth = 12;
const iterations = 300;

class Node {
    constructor(left, right) {
        this.left = left;
        this.right = right;
    }
}

function makeTree(d) {
    if (d === 0) {
        return new Node(null, null);
    }
    return new Node(makeTree(d - 1), makeTree(d - 1));
}

function countNodes(node) {
    if (node === null) {
        return 0;
    }
    return 1 + countNodes(node.left) + countNodes(node.right);
}

const t0 = performance.now();
let total = 0;
for (let i = 0; i < iterations; i++) {
    total += countNodes(makeTree(depth));
}
const dt = performance.now() - t0;

check("binary_trees.total", total, 2457300);
check("binary_trees.nodes", total, iterations * 8191);

console.log("value checksum: " + total);
console.log("value per_tree: " + (2 ** (depth + 1) - 1));
console.log("bench-time: " + dt + " ms");

function check(label, got, want) {
    if (got !== want) {
        console.error("checksum drift: " + label + " got " + got + " want " + want);
        process.exit(1);
    }
}
