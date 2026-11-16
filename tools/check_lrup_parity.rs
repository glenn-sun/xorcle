//! LRUP(⊕) proof checker for the extended DIMACS/XNF format, CNF-XOR lines included.
//!
//! Usage: check_lrup_parity <input.xnf> <proof.lrup>
//!
//! The proof may be in the text or the binary format, told apart by its first byte.
//!
//! Every equation is stored as its linear negation, a row `r` of `n + 1` bits standing for
//! `r · (x, 1) = 0`: bits `[0, n)` are coefficients and bit `n` the constant. So the token `1+2`,
//! `x1 + x2 = 1`, is stored as the row of `x1 + x2 = 0`, and a row of all zeros stands for an
//! equation that is false.

use std::collections::HashMap;
use std::io::BufRead;
use std::process::ExitCode;

#[derive(Clone, PartialEq)]
struct Bits(Vec<u64>);

impl Bits {
    fn zeros(num_vars: usize) -> Bits {
        Bits(vec![0; (num_vars + 1).div_ceil(64)])
    }

    fn get(&self, i: usize) -> bool {
        (self.0[i / 64] >> (i % 64)) & 1 == 1
    }

    fn flip(&mut self, i: usize) {
        self.0[i / 64] ^= 1 << (i % 64);
    }

    fn plus_assign(&mut self, other: &Bits) {
        for (a, b) in self.0.iter_mut().zip(&other.0) {
            *a ^= b;
        }
    }

    fn is_zero(&self) -> bool {
        self.0.iter().all(|&word| word == 0)
    }

    fn first_one(&self) -> Option<usize> {
        for (word_idx, &word) in self.0.iter().enumerate() {
            if word != 0 {
                return Some(word_idx * 64 + word.trailing_zeros() as usize);
            }
        }
        None
    }
}

/// A row-echelon basis over GF(2). Each row is zero at the pivots of the rows before it, so
/// reducing by the rows in order clears every pivot for good.
struct Basis {
    rows: Vec<(usize, Bits)>,
}

impl Basis {
    fn reduce(&self, mut row: Bits) -> Bits {
        for (pivot, pivot_row) in &self.rows {
            if row.get(*pivot) {
                row.plus_assign(pivot_row);
            }
        }
        row
    }

    /// Adds `row` unless the basis spans it; returns whether it was added.
    fn add(&mut self, row: Bits) -> bool {
        let row = self.reduce(row);
        match row.first_one() {
            None => false,
            Some(pivot) => {
                self.rows.push((pivot, row));
                true
            }
        }
    }
}

enum Step {
    Conflict,
    Deduced,
    AlreadyKnown,
    NotUnit,
}

/// One unit-propagation step of a clause against the facts the basis spans.
fn propagate(clause: &[Bits], basis: &mut Basis, num_vars: usize) -> Step {
    // The clause's equations that the facts do not already make false, each reduced.
    let mut remaining: Vec<Bits> = Vec::new();
    for row in clause {
        let reduced = basis.reduce(row.clone());
        if !reduced.is_zero() {
            remaining.push(reduced);
        }
    }

    // Every equation is false: a conflict.
    let Some(first) = remaining.first() else {
        return Step::Conflict;
    };

    // The clause is unit only when every remaining equation is the same one modulo the facts.
    if remaining.iter().any(|row| row != first) {
        return Step::NotUnit;
    }

    // That equation must hold, so the linear negation of its row is a fact, a new one unless
    // the facts already make the equation true.
    let mut equation = first.clone();
    equation.flip(num_vars);
    if basis.add(equation) {
        Step::Deduced
    } else {
        Step::AlreadyKnown
    }
}

/// An XNF equation token such as `-1+2+3`; a leading `-` sets the constant bit.
fn parse_equation(token: &str, num_vars: usize) -> Result<Bits, String> {
    let (constant, body) = match token.strip_prefix('-') {
        Some(body) => (true, body),
        None => (false, token),
    };
    if body.is_empty() {
        return Err(format!("Invalid token: '{token}'"));
    }
    if body.contains('-') {
        return Err(format!("Illegal '-' inside token: '{token}'"));
    }
    let mut row = Bits::zeros(num_vars);
    for part in body.split('+') {
        if part.is_empty() {
            return Err(format!("Empty variable in token: '{token}'"));
        }
        let var: usize = match part.parse() {
            Ok(var) => var,
            Err(_) => return Err(format!("Invalid integer in token: '{token}'")),
        };
        if var == 0 || var > num_vars {
            return Err(format!("Variable out of range in token: '{token}'"));
        }
        row.flip(var - 1); // addition, so a repeated variable cancels
    }
    if constant {
        row.flip(num_vars);
    }
    Ok(row)
}

/// A CNF-XOR line `x<lit> ... 0`, its leading `x` removed. The literals XOR to 1, so the
/// constant bit is set when an odd number of them are negated.
fn parse_xor_line(body: &str, num_vars: usize) -> Result<Bits, String> {
    let tokens: Vec<&str> = body.split_whitespace().collect();
    let Some((&"0", literals)) = tokens.split_last() else {
        return Err(format!("XOR line must end with its only 0: 'x{body}'"));
    };
    if literals.contains(&"0") {
        return Err(format!("XOR line must end with its only 0: 'x{body}'"));
    }
    let mut row = Bits::zeros(num_vars);
    for token in literals {
        let lit: i64 = match token.parse() {
            Ok(lit) => lit,
            Err(_) => return Err(format!("Invalid literal in XOR line: '{token}'")),
        };
        let var = lit.unsigned_abs() as usize;
        if var == 0 || var > num_vars {
            return Err(format!("Variable out of range in XOR line: '{token}'"));
        }
        row.flip(var - 1);
        if lit < 0 {
            row.flip(num_vars);
        }
    }
    Ok(row)
}

fn is_skipped(line: &str) -> bool {
    line.is_empty() || line.starts_with('c')
}

/// The number of variables and the input clauses, numbered from 1.
fn parse_input(text: &str) -> Result<(usize, HashMap<u64, Vec<Bits>>), String> {
    // The trimmed lines that are neither blank nor a comment.
    let mut lines = text.lines().map(str::trim).filter(|line| !is_skipped(line));

    let header = lines.next().ok_or("No header found")?;
    if !header.starts_with('p') {
        return Err(format!("Expected header, got: {header}"));
    }
    let parts: Vec<&str> = header.split_whitespace().collect();
    let bad_header = || format!("Invalid header: {header}");
    if parts.len() < 4 || !matches!(parts[1], "cnf" | "xnf") {
        return Err(bad_header());
    }
    let num_vars: usize = parts[2].parse().map_err(|_| bad_header())?;
    parts[3].parse::<i64>().map_err(|_| bad_header())?;

    let mut clauses = HashMap::new();
    // The equations of a clause whose closing `0` has not been read yet.
    let mut current = Vec::new();
    for line in lines {
        if let Some(body) = line.strip_prefix('x') {
            if !current.is_empty() {
                return Err("Clause missing trailing 0 before an XOR line".into());
            }
            let id = clauses.len() as u64 + 1;
            clauses.insert(id, vec![parse_xor_line(body, num_vars)?]);
            continue;
        }
        for token in line.split_whitespace() {
            if token == "0" {
                let id = clauses.len() as u64 + 1;
                clauses.insert(id, std::mem::take(&mut current));
            } else {
                current.push(parse_equation(token, num_vars)?);
            }
        }
    }
    if !current.is_empty() {
        return Err("Last clause missing trailing 0".into());
    }
    Ok((num_vars, clauses))
}

fn parse_ids(tokens: &[&str]) -> Result<Vec<u64>, String> {
    let mut ids = Vec::new();
    for token in tokens {
        match token.parse::<i64>() {
            Ok(id) if id > 0 => ids.push(id as u64),
            Ok(id) => return Err(format!("Clause id must be positive: {id}")),
            Err(_) => return Err(format!("Invalid clause id: '{token}'")),
        }
    }
    Ok(ids)
}

enum ProofLine {
    Deletion(Vec<u64>),
    Learned {
        id: u64,
        rows: Vec<Bits>,
        reason_ids: Vec<u64>,
    },
}

/// A deletion line is `<id> ... 0`, with one `0` and at least one id; a learned line is
/// `<new id> <equation> ... 0 <reason id> ... 0`, with exactly two `0` tokens and the second
/// one last.
fn parse_proof_line(tokens: &[&str], num_vars: usize) -> Result<ProofLine, String> {
    let Some((&"0", body)) = tokens.split_last() else {
        return Err("Proof line must end with 0".into());
    };
    let zero_idxs: Vec<usize> = (0..body.len()).filter(|&i| body[i] == "0").collect();
    match zero_idxs[..] {
        [] => {
            let ids = parse_ids(body)?;
            if ids.is_empty() {
                return Err("Deletion line lists no clause id".into());
            }
            Ok(ProofLine::Deletion(ids))
        }
        [0] => Err("Missing learned clause id".into()),
        [zero_idx] => {
            let id = parse_ids(&body[..1])?[0];
            let mut rows = Vec::new();
            for token in &body[1..zero_idx] {
                rows.push(parse_equation(token, num_vars)?);
            }
            let reason_ids = parse_ids(&body[zero_idx + 1..])?;
            Ok(ProofLine::Learned {
                id,
                rows,
                reason_ids,
            })
        }
        _ => Err("Proof line must contain exactly one or two zeros".into()),
    }
}

/// Whether the negation of the clause with `rows` propagates to a conflict through the
/// reasons in the order given, each reason before the conflict unit, its equation possibly
/// already known; the error names what failed.
fn check_learned(
    id: u64,
    rows: &[Bits],
    reason_ids: &[u64],
    clauses: &HashMap<u64, Vec<Bits>>,
    num_vars: usize,
) -> Result<(), String> {
    // The facts the negated clause asserts: each of its equations is false.
    let mut basis = Basis { rows: Vec::new() };
    for row in rows {
        basis.add(row.clone());
    }
    for rid in reason_ids {
        let Some(reason) = clauses.get(rid) else {
            return Err(format!(
                "missing reason clause id {rid} for learned id {id}"
            ));
        };
        match propagate(reason, &mut basis, num_vars) {
            Step::Conflict => return Ok(()),
            Step::Deduced | Step::AlreadyKnown => {}
            Step::NotUnit => {
                return Err(format!(
                    "reason clause {rid} did not propagate for learned id {id}"
                ))
            }
        }
    }
    Err(format!("no conflict derived for learned id {id}"))
}

/// The clauses not yet deleted and what the lines so far have done.
struct Checker {
    num_vars: usize,
    clauses: HashMap<u64, Vec<Bits>>,
    max_id: u64,
    line_count: u64,
    learned_count: u64,
    deleted_count: u64,
    refuted: bool,
}

impl Checker {
    /// Checks one line and applies it; the error names what failed.
    fn apply(&mut self, line: ProofLine) -> Result<(), String> {
        self.line_count += 1;
        match line {
            ProofLine::Deletion(ids) => {
                for id in ids {
                    if self.clauses.remove(&id).is_none() {
                        return Err(format!(
                            "parse error: Deletion of clause id {id}, which does not exist"
                        ));
                    }
                    self.deleted_count += 1;
                }
            }
            ProofLine::Learned {
                id,
                rows,
                reason_ids,
            } => {
                if id <= self.max_id {
                    return Err(format!(
                        "learned id {id} does not exceed every earlier id ({})",
                        self.max_id
                    ));
                }
                check_learned(id, &rows, &reason_ids, &self.clauses, self.num_vars)?;
                // Rows of all zeros are equations that are false, so the clause is empty.
                self.refuted |= rows.iter().all(Bits::is_zero);
                self.clauses.insert(id, rows);
                self.max_id = id;
                self.learned_count += 1;
            }
        }
        Ok(())
    }

    fn summary(self) -> Result<String, String> {
        if !self.refuted {
            return Err("ERROR: no line derives the empty clause".into());
        }
        Ok(format!(
            "OK: lines={} learned={} deletions={} clauses_final={}",
            self.line_count,
            self.learned_count,
            self.deleted_count,
            self.clauses.len()
        ))
    }
}

/// Checks a text proof line by line.
fn check_text(proof: impl BufRead, checker: &mut Checker) -> Result<(), String> {
    for (line_idx, raw) in proof.lines().enumerate() {
        let lineno = line_idx + 1;
        let raw = raw.map_err(|e| format!("ERROR line {lineno}: cannot read: {e}"))?;
        let line = raw.trim();
        if is_skipped(line) {
            continue;
        }
        let tokens: Vec<&str> = line.split_whitespace().collect();
        let parsed = parse_proof_line(&tokens, checker.num_vars)
            .map_err(|msg| format!("ERROR line {lineno}: parse error: {msg}"))?;
        checker
            .apply(parsed)
            .map_err(|msg| format!("ERROR line {lineno}: {msg}"))?;
    }
    Ok(())
}

/// A binary proof and the count of its bytes read.
struct BinaryReader<R: BufRead> {
    inner: R,
    offset: u64,
}

impl<R: BufRead> BinaryReader<R> {
    /// The next byte, or `None` at the end of the proof.
    fn byte(&mut self) -> Result<Option<u8>, String> {
        let buffer = self
            .inner
            .fill_buf()
            .map_err(|e| format!("cannot read: {e}"))?;
        let Some(&byte) = buffer.first() else {
            return Ok(None);
        };
        self.inner.consume(1);
        self.offset += 1;
        Ok(Some(byte))
    }

    /// A number in seven-bit groups, the lowest first, each byte but the last with its high
    /// bit set.
    fn number(&mut self) -> Result<u64, String> {
        // A number that ends within the buffer's first nine bytes fits in 63 bits and is read
        // in place.
        let buffer = self
            .inner
            .fill_buf()
            .map_err(|e| format!("cannot read: {e}"))?;
        if let Some(last) = buffer.iter().take(9).position(|&byte| byte & 0x80 == 0) {
            let number = buffer[..=last]
                .iter()
                .rev()
                .fold(0u64, |number, &byte| number << 7 | u64::from(byte & 0x7f));
            self.inner.consume(last + 1);
            self.offset += last as u64 + 1;
            return Ok(number);
        }
        let mut number = 0u64;
        let mut shift = 0;
        loop {
            let byte = self.byte()?.ok_or("Proof ends inside a line")?;
            let group = u64::from(byte & 0x7f);
            if shift >= 64 || (group << shift) >> shift != group {
                return Err("Number exceeds 64 bits".into());
            }
            number |= group << shift;
            if byte & 0x80 == 0 {
                return Ok(number);
            }
            shift += 7;
        }
    }

    /// Numbers up to the `0` that ends the list.
    fn ids(&mut self) -> Result<Vec<u64>, String> {
        let mut ids = Vec::new();
        loop {
            match self.number()? {
                0 => return Ok(ids),
                id => ids.push(id),
            }
        }
    }

    /// An equation after its head `2 * k + c`: `k` variables, each written as its distance
    /// from the one before, the first from 0, and the constant bit `c`.
    fn equation(&mut self, head: u64, num_vars: usize) -> Result<Bits, String> {
        let num_row_vars = head / 2;
        if num_row_vars == 0 {
            return Err("Equation has no variable".into());
        }
        let mut row = Bits::zeros(num_vars);
        let mut var = 0u64;
        for _ in 0..num_row_vars {
            let gap = self.number()?;
            if gap == 0 {
                return Err("Equation repeats a variable".into());
            }
            var = var.saturating_add(gap);
            if var > num_vars as u64 {
                return Err(format!("Variable out of range: {var}"));
            }
            row.flip(var as usize - 1);
        }
        if head % 2 == 1 {
            row.flip(num_vars);
        }
        Ok(row)
    }

    /// The line after its leading byte: `a` for a learned clause, `d` for a deletion.
    fn line(&mut self, kind: u8, num_vars: usize) -> Result<ProofLine, String> {
        match kind {
            b'a' => {
                let id = self.number()?;
                if id == 0 {
                    return Err("Clause id must be positive: 0".into());
                }
                let mut rows = Vec::new();
                loop {
                    match self.number()? {
                        0 => break,
                        head => rows.push(self.equation(head, num_vars)?),
                    }
                }
                let reason_ids = self.ids()?;
                Ok(ProofLine::Learned {
                    id,
                    rows,
                    reason_ids,
                })
            }
            b'd' => {
                let ids = self.ids()?;
                if ids.is_empty() {
                    return Err("Deletion line lists no clause id".into());
                }
                Ok(ProofLine::Deletion(ids))
            }
            _ => Err(format!(
                "Line starts with byte 0x{kind:02x}, neither 'a' nor 'd'"
            )),
        }
    }
}

/// Checks a binary proof line by line.
fn check_binary(proof: impl BufRead, checker: &mut Checker) -> Result<(), String> {
    let mut reader = BinaryReader {
        inner: proof,
        offset: 0,
    };
    let mut lineno = 0u64;
    loop {
        let offset = reader.offset;
        let at = |lineno: u64| format!("ERROR line {lineno} (byte {offset})");
        let kind = match reader.byte() {
            Ok(Some(kind)) => kind,
            Ok(None) => return Ok(()),
            Err(msg) => return Err(format!("{}: {msg}", at(lineno + 1))),
        };
        lineno += 1;
        let parsed = reader
            .line(kind, checker.num_vars)
            .map_err(|msg| format!("{}: parse error: {msg}", at(lineno)))?;
        checker
            .apply(parsed)
            .map_err(|msg| format!("{}: {msg}", at(lineno)))?;
    }
}

/// Checks every line, and fails unless some learned line derives the empty clause. A new id
/// must exceed every id before it, so a text line cut short after its first `0` is refused as
/// a deletion of a clause that does not exist. A proof whose first byte is `a` or `d` is
/// binary; any other is text, which starts with a digit, a comment or a blank.
fn check_proof(
    mut proof: impl BufRead,
    num_vars: usize,
    clauses: HashMap<u64, Vec<Bits>>,
) -> Result<String, String> {
    let mut checker = Checker {
        num_vars,
        max_id: clauses.len() as u64,
        clauses,
        line_count: 0,
        learned_count: 0,
        deleted_count: 0,
        refuted: false,
    };
    let first = proof
        .fill_buf()
        .map_err(|e| format!("ERROR: cannot read the proof: {e}"))?
        .first()
        .copied();
    if matches!(first, Some(b'a' | b'd')) {
        check_binary(proof, &mut checker)?;
    } else {
        check_text(proof, &mut checker)?;
    }
    checker.summary()
}

fn run(args: &[String]) -> Result<String, String> {
    let [input_path, proof_path] = args else {
        return Err("Usage: check_lrup_parity <input.xnf> <proof.lrup>".into());
    };
    let cannot_read = |path: &str, e: std::io::Error| format!("ERROR: cannot read {path}: {e}");

    let input = std::fs::read_to_string(input_path).map_err(|e| cannot_read(input_path, e))?;
    let (num_vars, clauses) = parse_input(&input).map_err(|e| format!("ERROR: input: {e}"))?;

    let proof = std::fs::File::open(proof_path).map_err(|e| cannot_read(proof_path, e))?;
    check_proof(std::io::BufReader::new(proof), num_vars, clauses)
}

fn main() -> ExitCode {
    let args: Vec<String> = std::env::args().skip(1).collect();
    match run(&args) {
        Ok(summary) => {
            println!("{summary}");
            ExitCode::SUCCESS
        }
        Err(error) => {
            eprintln!("{error}");
            ExitCode::FAILURE
        }
    }
}
