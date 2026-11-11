"""Mutation tests for the check_lrup_parity binary: each case of tests/lrup_cases.txt passes or
fails as it says, a failure with the message given here, in the text format and again in the
binary one; and the binary format's own malformations fail with theirs.

Usage: test_check_lrup_parity.py <check_lrup_parity binary> [unittest options]
"""

import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
CHECKER = ""

#: What the checker says about each broken case.
MESSAGES = {
    "dropped_reason": "no conflict derived",
    # The flipped unit makes reason 1 unit with its equation already known.
    "flipped_literal": "no conflict derived",
    "satisfied_reason_not_unit": "reason clause 4 did not propagate",
    "implied_reason_not_unit": "reason clause 2 did not propagate",
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

#: The cases whose proofs have no binary spelling, their lines being malformed as text.
TEXT_ONLY = {"line_not_ending_in_zero", "three_zeros"}

#: Binary proofs for the instance (x1 or x2), not x1, not x2, and what the checker says.
BINARY_MALFORMED = {
    "no_variable": (b"a\x04\x01\x00\x03\x01\x00", "Equation has no variable"),
    "repeated_variable": (b"a\x04\x04\x01\x00\x00\x03\x01\x00", "Equation repeats a variable"),
    "variable_out_of_range": (b"a\x04\x02\x03\x00\x03\x01\x00", "Variable out of range"),
    "cut_inside_a_line": (b"a\x04\x02\x01\x00\x03", "Proof ends inside a line"),
    "cut_inside_a_number": (b"a\x84", "Proof ends inside a line"),
    "unknown_line": (b"a\x04\x02\x01\x00\x03\x01\x00x\x03\x00", "neither 'a' nor 'd'"),
    "number_past_64_bits": (b"d" + b"\xff" * 9 + b"\x7f\x00", "Number exceeds 64 bits"),
}
X1_OR_X2 = "p xnf 2 3\n1 2 0\n-1 0\n-2 0\n"


def number(value: int) -> bytes:
    """Seven bits a byte, the lowest first, the high bit on every byte but the last."""
    out = bytearray()
    while value >= 0x80:
        out.append(value & 0x7F | 0x80)
        value >>= 7
    out.append(value)
    return bytes(out)


def to_binary(text: str) -> bytes:
    """The binary spelling of a text proof whose every line ends in its one or two zeros."""
    out = bytearray()
    for line in text.splitlines():
        tokens = line.split()
        if not tokens or tokens[0].startswith("c"):
            continue
        zeros = [i for i, token in enumerate(tokens) if token == "0"]
        if len(zeros) == 1:
            out += b"d" + b"".join(number(int(t)) for t in tokens[:-1]) + b"\0"
            continue
        out += b"a" + number(int(tokens[0]))
        for token in tokens[1 : zeros[0]]:
            variables = [int(v) for v in token.lstrip("-").split("+")]
            out += number(2 * len(variables) + token.startswith("-"))
            previous = 0
            for var in variables:
                out += number(var - previous)
                previous = var
        out += b"\0" + b"".join(number(int(t)) for t in tokens[zeros[0] + 1 : -1]) + b"\0"
    return bytes(out)


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


def run_checker(tmp: str, instance: str, proof: bytes) -> subprocess.CompletedProcess:
    instance_path = os.path.join(tmp, "instance.xnf")
    proof_path = os.path.join(tmp, "proof.lrup")
    with open(instance_path, "w", encoding="utf-8") as f:
        f.write(instance)
    with open(proof_path, "wb") as f:
        f.write(proof)
    return subprocess.run([CHECKER, instance_path, proof_path], capture_output=True, text=True)


class CheckLrupParityTest(unittest.TestCase):
    def check_cases(self, binary: bool) -> None:
        cases = read_cases(os.path.join(HERE, "lrup_cases.txt"))
        self.assertEqual({name for name, passes, _, _ in cases if not passes}, set(MESSAGES))
        with tempfile.TemporaryDirectory() as tmp:
            for name, passes, instance, proof in cases:
                if binary and name in TEXT_ONLY:
                    continue
                with self.subTest(name):
                    spelled = to_binary(proof) if binary else proof.encode()
                    run = run_checker(tmp, instance, spelled)
                    out = run.stdout + run.stderr
                    if passes:
                        self.assertEqual(run.returncode, 0, out)
                        self.assertIn("OK:", out)
                    else:
                        self.assertEqual(run.returncode, 1, out)
                        self.assertIn(MESSAGES[name], out)

    def test_shared_cases(self) -> None:
        self.check_cases(binary=False)

    def test_shared_cases_in_binary(self) -> None:
        self.check_cases(binary=True)

    def test_binary_malformed(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            for name, (proof, message) in BINARY_MALFORMED.items():
                with self.subTest(name):
                    run = run_checker(tmp, X1_OR_X2, proof)
                    out = run.stdout + run.stderr
                    self.assertEqual(run.returncode, 1, out)
                    self.assertIn(message, out)


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    CHECKER = sys.argv.pop(1)
    unittest.main()
