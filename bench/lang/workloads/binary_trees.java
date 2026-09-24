// binary_trees 参照端口(与 binary_trees.aria 同算法同规模)。
class Bench_binary_trees {
    static class Node {
        Node left;
        Node right;
    }

    static Node makeTree(int d) {
        Node node = new Node();
        if (d > 0) {
            node.left = makeTree(d - 1);
            node.right = makeTree(d - 1);
        }
        return node;
    }

    static long countNodes(Node node) {
        if (node == null) {
            return 0;
        }
        return 1 + countNodes(node.left) + countNodes(node.right);
    }

    static long[] pass() {
        int depth = 12;
        long total = 0;
        for (int i = 0; i < 300; i++) {
            total += countNodes(makeTree(depth));
        }
        return new long[] {total, countNodes(makeTree(depth))};
    }

    static void check(String label, long got, long want) {
        if (got != want) {
            System.err.println("checksum drift: " + label + " got " + got + " want " + want);
            System.exit(1);
        }
    }

    public static void main(String[] args) {
        long[] warm = pass();
        long t0 = System.nanoTime();
        long[] v = pass();
        double ms = (System.nanoTime() - t0) / 1e6;
        check("warmup stable", warm[0], v[0]);
        check("checksum", v[0], 2457300L);
        check("nodes", v[0], 300L * 8191);
        check("per_tree", v[1], 8191L);
        System.out.println("value checksum: " + v[0]);
        System.out.println("value per_tree: " + v[1]);
        System.out.println("bench-time: " + ms + " ms");
    }
}
