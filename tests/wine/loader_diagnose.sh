#!/bin/sh
# What the Wine loader does when it does not return.
#
# The loader builds, is a correct arm64 Mach-O, and prints nothing at all before
# being killed. A process that has produced no output is blocked before its first
# line, so the answer is in the kernel or in the loader's own startup rather than in
# anything Wine would have said.
#
# The first version of this script wrote the state into the same file as the later
# sections, and the state went missing from the artifact while the status said 137.
# This version keeps one directory per run, writes each measurement into its own
# file, and assembles the log last from those files, so no section can be absent
# without the file itself being absent. Nothing is inferred from a command that may
# not have run.
#
# Usage: loader_diagnose.sh <loader-relative-path> <wine-build-dir> <output-dir>
#
# No `set -e`: every probe here is allowed to fail, and what is not allowed is
# failing silently.

loader="$1"
build="$2"
out="$3"

if [ -z "$loader" ] || [ -z "$build" ] || [ -z "$out" ]; then
  echo "usage: $0 <loader> <build-dir> <output-dir>" >&2
  exit 2
fi
mkdir -p "$out"
cd "$build" || exit 2

echo "== what is being diagnosed =="
ls -la "$loader" 2>&1 || echo "MISSING: $loader"
file "$loader" 2>&1 || true
otool -L "$loader" 2>&1 || true
echo
echo "== the modules the loader has to find =="
for module in dlls/ntdll/ntdll.so dlls/win32u/win32u.so server/wineserver; do
  if [ -e "$module" ]; then ls -la "$module"; else echo "$module: MISSING"; fi
done

pe_dirs=$(find dlls -maxdepth 2 -type d -name '*-windows' 2>/dev/null | sort | tr '\n' ':')
dllpath="dlls/ntdll:dlls/win32u:$pe_dirs"
echo
echo "== sample(1) available: $(command -v sample >/dev/null 2>&1 && echo yes || echo no) =="

env_snapshot() {
  echo "WINEDEBUG=${WINEDEBUG-<unset>}"
  echo "WINEDLLPATH=${WINEDLLPATH-<unset>}"
  echo "WINEPREFIX=${WINEPREFIX-<unset>}"
  echo "WINELOADER=${WINELOADER-<unset>}"
  echo "DYLD_PRINT_INITIALIZERS=${DYLD_PRINT_INITIALIZERS-<unset>}"
  echo "DYLD_PRINT_LIBRARIES=${DYLD_PRINT_LIBRARIES-<unset>}"
  echo "cwd=$(pwd)"
}

# Run one configuration, watch it, and if it is still alive ask it where it is
# while it is still there to be asked.
run_case() {
  label="$1"
  seconds="$2"
  shift 2
  dir="$out/$label"
  mkdir -p "$dir"
  : > "$dir/stdout"
  : > "$dir/stderr"
  : > "$dir/sample.txt"
  : > "$dir/state"
  : > "$dir/ps.txt"
  : > "$dir/lsof.txt"
  : > "$dir/wine-tmp.txt"
  env_snapshot > "$dir/env.txt"

  echo "--- $label: $*"
  "$@" > "$dir/stdout" 2> "$dir/stderr" &
  pid=$!

  waited=0
  while [ "$waited" -lt "$seconds" ] && kill -0 "$pid" 2>/dev/null; do
    sleep 1
    waited=$((waited + 1))
  done

  if kill -0 "$pid" 2>/dev/null; then
    echo "STILL RUNNING after ${seconds}s; stopped by this script" > "$dir/state"
    # %cpu and cpu time separate a process blocked in the kernel from one spinning.
    ps -o pid,ppid,stat,%cpu,time,wchan,command -p "$pid" > "$dir/ps.txt" 2>&1 || true
    ls -l /tmp 2>/dev/null | grep -i wine > "$dir/wine-tmp.txt" 2>&1 || true
    lsof -p "$pid" > "$dir/lsof.txt" 2>&1 || true
    if command -v sample >/dev/null 2>&1; then
      sample "$pid" 3 -file "$dir/sample.txt" > "$dir/sample.log" 2>&1 || true
    else
      echo "sample(1) is not available on this machine" > "$dir/sample.txt"
    fi
    kill -9 "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    echo 137 > "$dir/status"
  else
    wait "$pid"
    rc=$?
    echo "exited with status $rc" > "$dir/state"
    echo "$rc" > "$dir/status"
  fi

  {
    echo "== $label"
    echo "argv: $*"
    cat "$dir/env.txt"
    echo "state: $(cat "$dir/state")"
    echo "status: $(cat "$dir/status")"
    echo "--- stdout ($(wc -c < "$dir/stdout" | tr -d ' ') bytes)"
    cat "$dir/stdout"
    echo "--- stderr ($(wc -c < "$dir/stderr" | tr -d ' ') bytes)"
    cat "$dir/stderr"
    echo "--- ps -o pid,ppid,stat,%cpu,time,wchan,command"
    cat "$dir/ps.txt"
    echo "--- wine files in /tmp"
    cat "$dir/wine-tmp.txt"
    echo "--- lsof -p (first 25 lines)"
    head -25 "$dir/lsof.txt"
    echo "--- sample: $(wc -c < "$dir/sample.txt" | tr -d ' ') bytes"
    head -45 "$dir/sample.txt"
  } > "$dir/log.txt"

  echo "    $(cat "$dir/state"); stdout $(wc -c < "$dir/stdout" | tr -d ' ') bytes, stderr $(wc -c < "$dir/stderr" | tr -d ' ') bytes"
}

echo
echo "== 1. plain, the way the earlier step ran it =="
( unset WINEDLLPATH; unset WINEPREFIX; unset DYLD_PRINT_INITIALIZERS; unset DYLD_PRINT_LIBRARIES; run_case plain 30 "./$loader" --version )

echo "== 2. with dyld tracing, to see whether the dynamic loader reaches Wine at all =="
( unset WINEDLLPATH; unset WINEPREFIX; WINEDEBUG=-all DYLD_PRINT_INITIALIZERS=1 DYLD_PRINT_LIBRARIES=1 run_case dyld_trace 30 "./$loader" --version )

echo "== 3. with WINEDLLPATH naming this build tree's modules =="
( unset WINEPREFIX; WINEDEBUG=-all WINEDLLPATH="$dllpath" run_case with_dllpath 30 "./$loader" --version )

echo "== 4. with a prefix that already exists =="
( mkdir -p "$out/prefix"; WINEDEBUG=-all WINEDLLPATH="$dllpath" WINEPREFIX="$out/prefix" run_case with_prefix 30 "./$loader" --version )

echo "== 5. --help, in case only the argument path is at fault =="
( unset WINEPREFIX; WINEDEBUG=-all WINEDLLPATH="$dllpath" run_case help 30 "./$loader" --help )

echo
echo "== statuses =="
ran=""
for case_dir in "$out"/plain "$out"/dyld_trace "$out"/with_dllpath "$out"/with_prefix "$out"/help; do
  [ -d "$case_dir" ] || continue
  label=$(basename "$case_dir")
  status=$(cat "$case_dir/status" 2>/dev/null || echo "?")
  bytes=$(wc -c < "$case_dir/stdout" 2>/dev/null | tr -d ' ')
  printf '%-13s status=%-5s stdout=%s bytes  %s\n' "$label" "$status" "${bytes:-0}" "$(cat "$case_dir/state" 2>/dev/null)"
  if [ "$status" = "0" ] && [ "${bytes:-0}" -gt 0 ]; then ran="$ran $label"; fi
done

if [ -n "$ran" ]; then
  echo "verdict: the loader returned with output in:$ran" | tee "$out/verdict.txt"
  exit 0
fi
echo "verdict: the loader produced no output and did not return in any configuration" | tee "$out/verdict.txt"
exit 1
