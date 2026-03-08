# CDCL with F2 linear algebra

Instructions for macOS on Apple Silicon: run `make all` from project root. For basic tests, run `make test`. For more instructions, run `./bin/main --help` after compiling.

For manual XNF conversions, use `python3 tools/xnf_convert.py <format> <input.xnf> <output>`, where `<format>` is one of `cnf`, `cnf-exp`, `cnf-xor`, or `2-xnf`. The `cnf-exp` mode is the no-new-vars exponential CNF encoding used by the benchmark workflow.
