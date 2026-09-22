#!/bin/sh
# What the Wine loader does when it does not return.
#
# The loader builds, is a correct arm64 Mach-O, and prints nothing at all before
# being killed at 300 seconds. A process that produces no output and does not exit
# is blocked before its first line, and the reason is in the kernel or in the
# loader's own startup, not in anything Wine would have said.
#
# So this script does four things the earlier step did not:
#
#   1. It waits a short, fixed time and then looks at where the process actually
#      is: ps for the state, lsof for the files and sockets it holds, and sample(1)
#      for a stack trace of what it is executing while stuck.
#   2. It keeps stdout and stderr separate, so "printed nothing" is a fact rather
#      than an absence.
#   3. It runs the same command in several configurations and records each one, so
#      a difference between them is evidence about the cause instead of a guess.
#   4. It writes a status file per configuration. It never exits non-zero because a
#      probe failed: the caller computes the verdict from the statuses, and a
#      verdict computed from evidence is the only kind this project takes.
#
# Usage: loader_diagnose.sh <loader-relative-path> <wine-build-dir> <output-dir>
#
# Deliberately no `set -e`: every probe here is allowed to fail. What is not
# allowed is failing silently.

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
echo "loader: $build/$loader"
ls -la "$loader" 2>&1 || echo "the loader path does not exist"
file "$loader" 2>&1 || true
echo "otool -L:"
otool -L "$loader" 2>&1 || true

# The Unix modules the loader has to find, and where they are.
echo
echo "== the modules the loader needs =="
for module in dlls/ntdll/ntdll.so dlls/win32u/win32u.so server/wineserver; do
  if [ -e "$module" ]; then
    ls -la "$module"
    file "$module"
  else
    echo "$module: MISSING"
  fi
done

# Wine is run from a build tree here, which is not the layout it searches by
# default. Both halves are given: the directory holding the unix .so files, and the
# directories holding the PE modules.
pe_dirs=$(find dlls -maxdepth 2 -type d -name '*-windows' 2>/dev/null | sort | tr '\n' ':')
unix_dirs="dlls/ntdll:dlls/win32u"
dllpath="$unix_dirs:$pe_dirs"
echo
echo "== WINEDLLPATH for the build tree =="
echo "$dllpath" | tr ':' '\n' | sed '/^$/d' | while read -r d; do
  printf '  %s (%s files)\n' "$d" "$(ls "$d" 2>/dev/null | wc -l | tr -d ' ')"
done

# One configuration: run it, wait, and if it is still alive say where it is.
# seconds=0 means wait until it exits.
probe() {
  label="$1"
  seconds="$2"
  shift 2

  stdout="$out/$label.stdout"
  stderr="$out/$label.stderr"
  log="$out/$label.txt"
  : > "$stdout"
  : > "$stderr"
  {
    echo "label: $label"
    echo "argv: $*"
    echo "WINEDEBUG=${WINEDEBUG-<unset>}"
    echo "WINEDLLPATH=${WINEDLLPATH-<unset>}"
    echo "WINEPREFIX=${WINEPREFIX-<unset>}"
    echo "WINELOADER=${WINELOADER-<unset>}"
    echo "cwd: $(pwd)"
  } > "$log"

  "$@" > "$stdout" 2> "$stderr" &
  pid=$!

  waited=0
  while [ "$waited" -lt "$seconds" ] && kill -0 "$pid" 2>/dev/null; do
    sleep 1
    waited=$((waited + 1))
  done

  if kill -0 "$pid" 2>/dev/null; then
    echo "state: STILL RUNNING after ${seconds}s" >> "$log"
    {
      echo "--- ps -o pid,ppid,stat,wchan,command:"
      ps -o pid,ppid,stat,wchan,command -p "$pid" 2>&1
    } >> "$log"
    {
      echo "--- what it has open (lsof -p):"
      lsof -p "$pid" 2>&1 | head -40
    } > "$out/$label.lsof.txt"
    if command -v sample >/dev/null 2>&1; then
      echo "--- sample $pid 3 (a stack trace of where it is stuck):" >> "$log"
      sample "$pid" 3 -file "$out/$label.sample.txt" >> "$log" 2>&1 || true
      if [ -f "$out/$label.sample.txt" ]; then
        echo "--- the frames it is executing:" >> "$log"
        grep -A12 'Call graph' "$out/$label.sample.txt" 2>/dev/null | head -20 >> "$log"
      fi
    else
      echo "sample(1) is not available on this machine" >> "$log"
    fi
    kill -9 "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
    echo "137" > "$out/$label.status"
    echo "status: 137 (SIGKILL from this script's own limit, so the number says the process did not return and nothing more)"
  else
    wait "$pid"
    rc=$?
    echo "$rc" > "$out/$label.status"
    echo "state: exited on its own with status $rc"
  fi

  {
    echo "--- stdout ($(wc -c < "$stdout" | tr -d ' ') bytes):"
    cat "$stdout"
    echo "--- stderr ($(wc -c < "$stderr" | tr -d ' ') bytes):"
    cat "$stderr"
  } >> "$log"
}

echo
echo "== configuration 1: the way the earlier step ran it, no module path =="
( unset WINEDLLPATH; unset WINEPREFIX; probe plain 30 "./$loader" --version )

echo
echo "== configuration 2: with WINEDEBUG=-all, to rule logging out =="
( unset WINEDLLPATH; unset WINEPREFIX; WINEDEBUG=-all probe debug_off 30 "./$loader" --version )

echo
echo "== configuration 3: with WINEDLLPATH pointing at this build tree =="
( unset WINEPREFIX; WINEDEBUG=-all WINEDLLPATH="$dllpath" probe with_dllpath 30 "./$loader" --version )

echo
echo "== configuration 4: a prefix that already exists, as a runtime would have =="
( mkdir -p "$out/prefix"; WINEDEBUG=-all WINEDLLPATH="$dllpath" WINEPREFIX="$out/prefix" probe with_prefix 30 "./$loader" --version )

echo
echo "== configuration 5: --help, in case the argument path is the problem =="
( unset WINEPREFIX; WINEDEBUG=-all WINEDLLPATH="$dllpath" probe help 30 "./$loader" --help )

echo
echo "== statuses =="
for status in "$out"/*.status; do
  [ -e "$status" ] || continue
  printf '%-14s %s\n' "$(basename "$status" .status)" "$(cat "$status")"
done

# One configuration returning 0 is the loader working, and configuration 3 is the
# one a runtime would use: a build tree with its module path set. The caller reads
# this file rather than re-deriving it.
if [ -f "$out/with_dllpath.status" ] && [ "$(cat "$out/with_dllpath.status")" = "0" ]; then
  echo "verdict: the loader runs when told where its modules are"
  exit 0
fi
if [ -f "$out/plain.status" ] && [ "$(cat "$out/plain.status")" = "0" ]; then
  echo "verdict: the loader runs with no module path, which is better than expected"
  exit 0
fi
echo "verdict: the loader did not return in any configuration"
exit 1
