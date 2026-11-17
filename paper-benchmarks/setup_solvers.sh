#!/usr/bin/env bash
# Fetch the reference solvers at named release versions, verify every download by
# sha256, and build those without a usable upstream binary. Executables land in
# solvers/bin/; solvers.json records what each one is and how to run it. Xorcle 0.2
# is built from this repository into ../build, which also holds the proof checker,
# and Xorcle 0.1 from the repository's tag v0.1. `encoders` fetches the two
# upstream encoders random/generate.py runs into random/encoders/; neither
# repository carries a licence, so they are fetched, never committed.
#
#   setup_solvers.sh [xorcle|kissat|cadical|cms|cms513|xorricane|2xornado|encoders ...]   (default: all)
#
# Environment: JOBS (make -j, default 8), FORCE=1 to rebuild a solver whose binary
# exists, BUILD_ROOT (source trees and builds; default under $TMPDIR or /tmp).
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
SOLVERS=$HERE/solvers
DL=$SOLVERS/dl
SRC=${BUILD_ROOT:-${TMPDIR:-/tmp}/xorcle-paper-solvers}
BIN=$SOLVERS/bin
LIB=$SOLVERS/lib
JOBS=${JOBS:-8}
FORCE=${FORCE:-0}
mkdir -p "$DL" "$SRC" "$BIN" "$LIB"

log() { printf '[setup %s] %s\n' "$(date +%H:%M:%S)" "$*"; }

# fetch URL FILE SHA256: download into dl/ unless present, then verify.
fetch() {
  local url=$1 file=$DL/$2 sha=$3
  if [[ ! -f $file ]]; then
    log "download $url"
    curl -fsSL --retry 3 -o "$file.part" "$url"
    mv "$file.part" "$file"
  fi
  echo "$sha  $file" | sha256sum -c --quiet - || { log "sha256 mismatch: $file"; exit 1; }
}

# unpack ARCHIVE DIR: a fresh source tree, stripped of the archive's top directory.
unpack() {
  local archive=$DL/$1 dir=$SRC/$2
  rm -rf "$dir"; mkdir -p "$dir"
  tar xzf "$archive" -C "$dir" --strip-components=1
}

have() { [[ $FORCE != 1 && -x $BIN/$1 ]] && { log "$1 present, skipped"; return 0; } || return 1; }

# --- Xorcle 0.2, this repository; Xorcle 0.1, tag v0.1 (daa0125) --------------
xorcle() {
  local repo
  repo=$(cd "$HERE/.." && pwd)
  if ! have xorcle-0.2; then
    cmake -S "$repo" -B "$repo/build" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$repo/build" -j"$JOBS"
    install -m 755 "$repo/build/xorcle" "$BIN/xorcle-0.2"
  fi
  if ! have xorcle-0.1; then
    local tag
    tag=$(git -C "$repo" rev-parse -q --verify 'refs/tags/v0.1^{commit}') \
      || { log "no tag v0.1: run git fetch origin tag v0.1"; exit 1; }
    [[ $tag == daa0125249200721a15a41959b7783ce3500d854 ]] || { log "v0.1 is $tag, not daa0125"; exit 1; }
    rm -rf "$SRC/xorcle-0.1"; mkdir -p "$SRC/xorcle-0.1"
    git -C "$repo" archive v0.1 | tar x -C "$SRC/xorcle-0.1"
    # v0.1 uses std::memcpy without including <cstring>, which libstdc++ does not supply.
    make -C "$SRC/xorcle-0.1" CXX="${CXX:-c++} -include cstring"
    install -m 755 "$SRC/xorcle-0.1/bin/xorcle" "$BIN/xorcle-0.1"
  fi
}

# --- Kissat rel-4.0.4 (8af8e56), upstream static binary --------------------------
kissat() {
  have kissat-4.0.4 && return
  fetch https://github.com/arminbiere/kissat/releases/download/rel-4.0.4/kissat-4.0.4-linux-amd64.zip \
        kissat-4.0.4-linux-amd64.zip 63282e5168ca040a2018edfba0726bf893a7a6bcdb1db87feb67857e2da697a7
  unzip -oq "$DL/kissat-4.0.4-linux-amd64.zip" -d "$SRC/kissat-4.0.4"
  install -m 755 "$SRC/kissat-4.0.4/kissat-4.0.4-linux-amd64" "$BIN/kissat-4.0.4"
}

# --- CaDiCaL rel-3.0.1 (c607304), no upstream binary: built ----------------------
cadical() {
  have cadical-3.0.1 && return
  fetch https://github.com/arminbiere/cadical/archive/refs/tags/rel-3.0.1.tar.gz \
        cadical-rel-3.0.1.tar.gz 0a8ea563b5a25f5aa064634814edab45cc0e45111ea0f5d412a565f806fd7e11
  unpack cadical-rel-3.0.1.tar.gz cadical-3.0.1
  (cd "$SRC/cadical-3.0.1" && ./configure && make -j"$JOBS")
  install -m 755 "$SRC/cadical-3.0.1/build/cadical" "$BIN/cadical-3.0.1"
}

# --- CryptoMiniSat release/v5.17.0 (03954f7), upstream static binary -------------
cms() {
  have cryptominisat5-5.17.0 && return
  fetch https://github.com/msoos/cryptominisat/releases/download/release/v5.17.0/cryptominisat5-v5.17.0-linux-amd64.tar.gz \
        cryptominisat5-v5.17.0-linux-amd64.tar.gz 474ff5cfaf77de7438dc063dbe4ccefcbb7dcf1e17de702c34ed88f5e026eda7
  mkdir -p "$SRC/cms-5.17.0"
  tar xzf "$DL/cryptominisat5-v5.17.0-linux-amd64.tar.gz" -C "$SRC/cms-5.17.0"
  install -m 755 "$SRC/cms-5.17.0/cryptominisat5" "$BIN/cryptominisat5-5.17.0"
}

# --- CryptoMiniSat release/5.13.0 (e60d035), upstream static binary --------------
# The version Xorcle v0.1's benchmarks used.
cms513() {
  have cryptominisat5-5.13.0 && return
  fetch https://github.com/msoos/cryptominisat/releases/download/release/5.13.0/cryptominisat5-linux-amd64.zip \
        cryptominisat5-5.13.0-linux-amd64.zip 7fecbcd8986cd63093bde2558298e522f52d9e2f1e473c806e993049143c4858
  unzip -oq "$DL/cryptominisat5-5.13.0-linux-amd64.zip" -d "$SRC/cms-5.13.0"
  install -m 755 "$SRC/cms-5.13.0/cryptominisat5" "$BIN/cryptominisat5-5.13.0"
}

# --- Xorricane 0.4.2.11 at 9fb3be9, the commit Xorricane-paper pins --------------
# No GitHub release exists; the pin is tag v0.4.2.11 plus a compiler-warning fix,
# a CMake policy line and a README line. Built as upstream's README says
# (`cmake . && make xorricane`, Release); its CMake clones m4ri at bb78acf itself.
xorricane() {
  have xorricane-0.4.2.11 && return
  fetch https://github.com/j-danner/Xorricane/archive/9fb3be90bdd3b952ca4467212fafd89fffc07e27.tar.gz \
        Xorricane-9fb3be9.tar.gz f51a49492ffa83cae7c5024bc6ac7eed80df1d6653a7ecf76fa5fc2c8a1c0978
  fetch https://github.com/j-danner/cryptominisat_no_cadiback/archive/6f09c60bb4dbdd9c4ba69d5bff4026564c2eedc1.tar.gz \
        cryptominisat_no_cadiback-6f09c60.tar.gz b5eaa5f80f47fb89c6e997c56b4ca31d75d928f930a61a890fffc77f9cff4443
  local dir=$SRC/xorricane-0.4.2.11
  unpack Xorricane-9fb3be9.tar.gz xorricane-0.4.2.11
  rmdir "$dir/src/cryptominisat" 2>/dev/null || rm -rf "$dir/src/cryptominisat"
  mkdir -p "$dir/src/cryptominisat"
  tar xzf "$DL/cryptominisat_no_cadiback-6f09c60.tar.gz" -C "$dir/src/cryptominisat" --strip-components=1
  (cd "$dir" && cmake . && make -j"$JOBS" xorricane)
  local m4ri_head
  m4ri_head=$(git -C "$dir/src/m4ri-src" rev-parse HEAD)
  [[ $m4ri_head == bb78acffaf9c2721e20bedd8ae55484d55b471d1 ]] || { log "m4ri at $m4ri_head, not bb78acf"; exit 1; }
  install -m 755 "$dir/xorricane" "$BIN/xorricane-0.4.2.11"
}

# --- 2-Xornado v1.0.2 (1681381), upstream binary ---------------------------------
# The release binary links libtbb.so.2 and libm4ri-0.0.20140914.so, which current
# distributions do not ship: TBB 2020.3's own binary provides the first, m4ri's
# release-20140914 the second (built). bin/2xornado-1.0.2 is a wrapper that points the loader at lib/.
xornado() {
  have 2xornado-1.0.2 && return
  fetch https://github.com/j-danner/2-Xornado/releases/download/v1.0.2/2xornado \
        2xornado-v1.0.2 063487f30def5a5f6f80919ece2c1dbfed9f98d4da5ce41ed6ffa28e13e22ad1
  fetch https://github.com/uxlfoundation/oneTBB/releases/download/v2020.3/tbb-2020.3-lin.tgz \
        tbb-2020.3-lin.tgz bb8cddd0277605d3ee7f4e19b138c983f298d69fcbb585385b59ef7239d5ef83
  fetch https://github.com/malb/m4ri/archive/refs/tags/release-20140914.tar.gz \
        m4ri-release-20140914.tar.gz 6887d9a24f83db6b9d78c63dbfb079464c4307deafd33dbe69e96c832f26ddc3
  local lib=$LIB/2xornado-1.0.2
  mkdir -p "$lib" "$SOLVERS/libexec"
  tar xzf "$DL/tbb-2020.3-lin.tgz" -C "$SRC" tbb/lib/intel64/gcc4.8/libtbb.so.2
  install -m 755 "$SRC/tbb/lib/intel64/gcc4.8/libtbb.so.2" "$lib/"
  unpack m4ri-release-20140914.tar.gz m4ri-20140914
  (cd "$SRC/m4ri-20140914" && autoreconf --install && ./configure --disable-png \
     && make -j"$JOBS")
  install -m 755 "$SRC/m4ri-20140914/.libs/libm4ri-0.0.20140914.so" "$lib/"
  install -m 755 "$DL/2xornado-v1.0.2" "$SOLVERS/libexec/2xornado-1.0.2"
  cat > "$BIN/2xornado-1.0.2" <<'EOF'
#!/bin/sh
here=$(dirname "$(readlink -f "$0")")
LD_LIBRARY_PATH=$here/../lib/2xornado-1.0.2${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} \
  exec "$here/../libexec/2xornado-1.0.2" "$@"
EOF
  chmod 755 "$BIN/2xornado-1.0.2"
}

# --- The random experiment's encoders, at pinned commits --------------------------
encoders() {
  local out=$HERE/random/encoders
  mkdir -p "$out/anf_to_2xnf"
  # Mate Soos's XNF to CNF-XOR translation, msoos/xnf-tests at 0a65a2c.
  local soos=https://raw.githubusercontent.com/msoos/xnf-tests/0a65a2cffce0587a105ceba942d993ec3ba94fe3
  fetch "$soos/xnf_to_xcnf.py" xnf_to_xcnf.py ad3b55b9ea52864b3482d328674cbdba778a36090b0eb1ad2a7cc7d04bb2b5fa
  # anf_to_2xnf (Wrazlmumfp/anf_to_2xnf at fa4b10e), whose Xnf.asCnf wrote Danner's .cnf files.
  local anf=https://raw.githubusercontent.com/Wrazlmumfp/anf_to_2xnf/fa4b10e0b508e417994f8acf3386a60e3760a7fa
  fetch "$anf/xnf.py" anf_to_2xnf-xnf.py 27d21439419cebba0d0d06deb53b852477042173601223c184a0c83a89d5827d
  fetch "$anf/anf.py" anf_to_2xnf-anf.py 413b458547b9dcdeac2bcb03d600188d84472c2fd98011712f8febf8b8660b20
  install -m 644 "$DL/xnf_to_xcnf.py" "$out/xnf_to_xcnf.py"
  install -m 644 "$DL/anf_to_2xnf-xnf.py" "$out/anf_to_2xnf/xnf.py"
  install -m 644 "$DL/anf_to_2xnf-anf.py" "$out/anf_to_2xnf/anf.py"
}

targets=("$@")
[[ ${#targets[@]} -gt 0 ]] || targets=(xorcle kissat cadical cms cms513 xorricane 2xornado encoders)
for t in "${targets[@]}"; do
  log "== $t"
  case $t in
    xorcle) xorcle ;; kissat) kissat ;; cadical) cadical ;; cms) cms ;; cms513) cms513 ;;
    xorricane) xorricane ;; 2xornado) xornado ;; encoders) encoders ;;
    *) log "unknown solver $t"; exit 2 ;;
  esac
done
log "done: $(ls "$BIN" | tr '\n' ' ')"
