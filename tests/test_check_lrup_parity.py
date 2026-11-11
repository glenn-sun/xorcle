"""Mutation tests for tools/check_lrup_parity.py: each case of tests/lrup_cases.txt passes or
fails as it says, a failure with the message given here."""

import contextlib
import io
import os
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "tools"))

import check_lrup_parity

#: What the checker says about each broken case.
MESSAGES = {
    "dropped_reason": "no conflict derived",
    "flipped_literal": "did not propagate",
    "reason_cited_after_its_deletion": "missing reason clause id 2",
    "missing_empty_clause": "no line derives the empty clause",
    "no_lines": "no line derives the empty clause",
    "duplicate_id": "does not exceed every earlier id",
    "reused_deleted_id": "does not exceed every earlier id",
    "unknown_id_deletion": "which does not exist",
    "double_deletion": "which does not exist",
    # A learned line cut after its first 0 reads as a deletion of the new id.
    "truncated_last_line": "which does not exist",
    "truncated_first_line": "which does not exist",
    "empty_deletion_line": "lists no clause id",
    "line_not_ending_in_zero": "must end with 0",
    "three_zeros": "exactly one or two zeros",
}


def read_cases(path: str) -> list[tuple[str, bool, str, str]]:
    """(name, passes, instance, proof) for each case of the shared file."""
    cases = []
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    i = 0
    while i < len(lines):
        if not lines[i].startswith("case "):
            i += 1
            continue
        _, name, expect = lines[i].split()
        proof_at = lines.index("proof", i)
        end_at = lines.index("end", proof_at)
        cases.append((name, expect == "pass", "\n".join(lines[i + 1 : proof_at]) + "\n",
                      "\n".join(lines[proof_at + 1 : end_at]) + "\n"))
        i = end_at + 1
    return cases


class CheckLrupParityTest(unittest.TestCase):
    def test_shared_cases(self) -> None:
        cases = read_cases(os.path.join(HERE, "lrup_cases.txt"))
        self.assertEqual({name for name, passes, _, _ in cases if not passes}, set(MESSAGES))
        with tempfile.TemporaryDirectory() as tmp:
            for name, passes, instance, proof in cases:
                with self.subTest(name):
                    instance_path = os.path.join(tmp, "instance.xnf")
                    proof_path = os.path.join(tmp, "proof.lrup")
                    with open(instance_path, "w", encoding="utf-8") as f:
                        f.write(instance)
                    with open(proof_path, "w", encoding="utf-8") as f:
                        f.write(proof)
                    out = io.StringIO()
                    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
                        status = check_lrup_parity.check_proof(instance_path, proof_path, verbose=False)
                    if passes:
                        self.assertEqual(status, 0, out.getvalue())
                        self.assertIn("OK:", out.getvalue())
                    else:
                        self.assertEqual(status, 1, out.getvalue())
                        self.assertIn(MESSAGES[name], out.getvalue())


if __name__ == "__main__":
    unittest.main()
