#!/usr/bin/env python3
"""Generate mutants of the sgpu driver by removing one safety check at a time.

Almost all mutants remove checks whose removal cannot corrupt kernel memory:
a bad command is at worst stopped by the device's own fault handling. The one
exception, unpublish_va, restores a real lifetime bug. Its test fails on the
first stale lookup, and the VM runs from a throwaway snapshot.

  mutate.py list                       print "<id> <test_ops>" for every mutant
  mutate.py apply <id> <in.c> <out.c>  write the mutated source
"""
import sys

# id, module test_ops value, [(original text, replacement), ...]
MUTANTS = [
    ("nop_fields", 1, [("if (c->len || c->src || c->dst)", "if (false)")]),
    ("fill_zero_len", 1, [("if (!c->len || c->len > SGPU_MAX_XFER || c->src > 0xff)",
                           "if (c->len > SGPU_MAX_XFER || c->src > 0xff)")]),
    ("fill_max_len", 1, [("if (!c->len || c->len > SGPU_MAX_XFER || c->src > 0xff)",
                          "if (!c->len || c->src > 0xff)")]),
    ("fill_pattern", 1, [("if (!c->len || c->len > SGPU_MAX_XFER || c->src > 0xff)",
                          "if (!c->len || c->len > SGPU_MAX_XFER)")]),
    ("copy_zero_len", 1, [("case SGPU_OP_COPY:\n\t\t\tif (!c->len || c->len > SGPU_MAX_XFER)",
                           "case SGPU_OP_COPY:\n\t\t\tif (c->len > SGPU_MAX_XFER)")]),
    ("range_end_same_bo", 1, [("if (!bo || ctx->page_bo[end >> SGPU_PAGE_SHIFT] != bo)", "if (!bo)")]),
    ("guard_page", 1, [("bo->npages + 1, 0);", "bo->npages, 0);"),
                       ("bitmap_set(ctx->va_used, first, bo->npages + 1);",
                        "bitmap_set(ctx->va_used, first, bo->npages);"),
                       ("bitmap_clear(ctx->va_used, bo->first_page, bo->npages + 1);",
                        "bitmap_clear(ctx->va_used, bo->first_page, bo->npages);")]),
    ("gate_test_hang", 0, [("case SGPU_OP_TEST_HANG:\n\t\t\tif (!test_ops)",
                            "case SGPU_OP_TEST_HANG:\n\t\t\tif (false)")]),
    ("gate_test_fault", 0, [("case SGPU_OP_TEST_FAULT:\n\t\t\tif (!test_ops)",
                             "case SGPU_OP_TEST_FAULT:\n\t\t\tif (false)")]),
    ("submit_pad", 1, [("if (args.pad || !args.count || args.count > SGPU_MAX_CMDS)",
                        "if (!args.count || args.count > SGPU_MAX_CMDS)")]),
    ("submit_zero_count", 1, [("if (args.pad || !args.count || args.count > SGPU_MAX_CMDS)",
                               "if (args.pad || args.count > SGPU_MAX_CMDS)")]),
    ("wait_foreign_fence", 1, [("if (!args.fence || args.fence > last)", "if (!args.fence)")]),
    ("wait_zero_fence", 1, [("if (!args.fence || args.fence > last)", "if (args.fence > last)")]),
    ("wait_max_timeout", 1, [("if (args.timeout_ms > SGPU_MAX_WAIT_MS)", "if (false)")]),
    ("create_pad", 1, [("if (args.pad || !args.size || args.size > SGPU_MAX_BO_SIZE)",
                        "if (!args.size || args.size > SGPU_MAX_BO_SIZE)")]),
    ("destroy_pad", 1, [("if (args.pad)\n\t\treturn -EINVAL;\n\n\tret = sgpu_enter(ctx->dev);",
                         "if (false)\n\t\treturn -EINVAL;\n\n\tret = sgpu_enter(ctx->dev);")]),
    ("unpublish_va", 1, [("\t\tctx->page_bo[bo->first_page + i] = NULL;\n", "\t\t;\n")]),
    ("xfer_pad", 1, [("if (args.pad || !args.len ||", "if (!args.len ||")]),
    ("xfer_zero_len", 1, [("if (args.pad || !args.len ||", "if (args.pad ||")]),
    ("banned_submit", 1, [("if (ctx->banned) {", "if (false) {")]),
    ("ban_on_reset", 1, [("guilty->banned = true;", "guilty->banned = false;")]),
    ("cancel_status", 1, [("job->ctx == guilty ? guilty_err : other_err", "guilty_err")]),
]


def main() -> int:
    if sys.argv[1] == "list":
        for name, test_ops, _ in MUTANTS:
            print(name, test_ops)
        return 0
    if sys.argv[1] == "apply":
        name, src, dst = sys.argv[2:5]
        pairs = next(m[2] for m in MUTANTS if m[0] == name)
        with open(src, encoding="utf-8") as f:
            text = f.read()
        for old, new in pairs:
            if text.count(old) != 1:
                print(f"{name}: pattern matched {text.count(old)} times: {old!r}", file=sys.stderr)
                return 1
            text = text.replace(old, new)
        with open(dst, "w", encoding="utf-8", newline="\n") as f:
            f.write(text)
        return 0
    return 2


if __name__ == "__main__":
    sys.exit(main())
