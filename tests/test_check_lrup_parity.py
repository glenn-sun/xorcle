"""Mutation tests for tools/check_lrup_parity.py: a tiny valid proof passes, each mutant fails."""

import contextlib
import io
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))

import check_lrup_parity

# (x1 or x2), not x1, not x2.
INSTANCE = "p xnf 2 3\n1 2 0\n-1 0\n-2 0\n"

# Learn x1 from clauses 3 and 1, delete 3, then the empty clause from 2 and 4.
VALID = "4 1 0 3 1 0\n3 0\n5 0 2 4 0\n"


class CheckLrupParityTest(unittest.TestCase):
    def setUp(self) -> None:
        self.dir = tempfile.TemporaryDirectory()
        self.instance = os.path.join(self.dir.name, "instance.xnf")
        with open(self.instance, "w", encoding="utf-8") as f:
            f.write(INSTANCE)

    def tearDown(self) -> None:
        self.dir.cleanup()

    def check(self, proof: str, instance: str | None = None) -> tuple[int, str]:
        instance_path = self.instance
        if instance is not None:
            instance_path = os.path.join(self.dir.name, "other.xnf")
            with open(instance_path, "w", encoding="utf-8") as f:
                f.write(instance)
        proof_path = os.path.join(self.dir.name, "proof.lrup")
        with open(proof_path, "w", encoding="utf-8") as f:
            f.write(proof)
        out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out):
            status = check_lrup_parity.check_proof(instance_path, proof_path, verbose=False)
        return status, out.getvalue()

    def assert_rejected(self, proof: str, message: str) -> None:
        status, output = self.check(proof)
        self.assertEqual(status, 1, output)
        self.assertIn(message, output)

    def test_valid_proof_passes(self) -> None:
        status, output = self.check(VALID)
        self.assertEqual(status, 0, output)
        self.assertIn("OK:", output)

    def test_comments_and_blank_lines_are_skipped(self) -> None:
        status, output = self.check("c a comment\n\n" + VALID)
        self.assertEqual(status, 0, output)

    def test_empty_clause_from_input_clauses_alone(self) -> None:
        status, output = self.check("4 0 2 3 1 0\n")
        self.assertEqual(status, 0, output)

    def test_xor_line_is_numbered_among_clauses(self) -> None:
        # x1 xor x2 = 1, then not x1 and not x2.
        status, output = self.check("4 0 2 3 1 0\n", instance="p cnf 2 2\nx1 2 0\n-1 0\n-2 0\n")
        self.assertEqual(status, 0, output)

    def test_dropped_reason_fails(self) -> None:
        self.assert_rejected("4 1 0 1 0\n3 0\n5 0 2 4 0\n", "no conflict derived")

    def test_flipped_literal_fails(self) -> None:
        self.assert_rejected("4 -1 0 3 1 0\n3 0\n5 0 2 4 0\n", "did not propagate")

    def test_reason_cited_after_its_deletion_fails(self) -> None:
        self.assert_rejected("4 1 0 3 1 0\n2 0\n5 0 2 4 0\n", "missing reason clause id 2")

    def test_missing_empty_clause_fails(self) -> None:
        self.assert_rejected("4 1 0 3 1 0\n3 0\n", "no line derives the empty clause")

    def test_no_lines_fails(self) -> None:
        self.assert_rejected("", "no line derives the empty clause")

    def test_duplicate_id_fails(self) -> None:
        self.assert_rejected("4 1 0 3 1 0\n4 0 2 4 0\n", "does not exceed every earlier id")

    def test_reused_deleted_id_fails(self) -> None:
        self.assert_rejected("4 1 0 3 1 0\n3 0\n3 0 2 4 0\n", "does not exceed every earlier id")

    def test_unknown_id_deletion_fails(self) -> None:
        self.assert_rejected("4 1 0 3 1 0\n9 0\n5 0 2 4 0\n", "which does not exist")

    def test_double_deletion_fails(self) -> None:
        self.assert_rejected("4 1 0 3 1 0\n3 0\n3 0\n5 0 2 4 0\n", "which does not exist")

    def test_truncated_learned_line_fails(self) -> None:
        # `5 0 2 4 0` cut after its first 0 reads as a deletion of the new id.
        self.assert_rejected("4 1 0 3 1 0\n3 0\n5 0\n", "which does not exist")
        # `4 1 0 3 1 0` cut after its first 0.
        self.assert_rejected("4 1 0\n", "which does not exist")

    def test_empty_deletion_line_fails(self) -> None:
        self.assert_rejected("0\n" + VALID, "lists no clause id")

    def test_line_not_ending_in_zero_fails(self) -> None:
        self.assert_rejected("4 1 0 3 1\n3 0\n5 0 2 4 0\n", "must end with 0")

    def test_three_zeros_fails(self) -> None:
        self.assert_rejected("4 1 0 3 0 1 0\n", "exactly one or two zeros")


if __name__ == "__main__":
    unittest.main()
