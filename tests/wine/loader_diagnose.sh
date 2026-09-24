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
  started=$(date +%s)
  "$@" > "$dir/stdout" 2> "$dir/stderr" &
  pid=$!

  # How long it lived is part of the answer: a process killed at exec time and one
  # killed after thirty seconds are different problems, and the previous version
  # recorded only the final number, which said neither.
  waited=0
  died_after=""
  while [ "$waited" -lt "$((seconds * 5))" ]; do
    if ! kill -0 "$pid" 2>/dev/null; then
      died_after=$(( $(date +%s) - started ))
      break
    fi
    # While it is alive, the whole process group matters: Wine's loader re-executes
    # itself on this platform, and a child that inherited the output file is a
    # different process from the one being waited on.
    if [ "$waited" = 4 ] || [ "$waited" = 10 ]; then
      pgid=$(ps -o pgid= -p "$pid" 2>/dev/null | tr -d ' ')
      if [ -n "$pgid" ]; then
        ps -o pid,ppid,pgid,stat,%cpu,time,command -g "$pgid" >> "$dir/ps.txt" 2>&1 || true
        echo "--- process group $pgid listed above" >> "$dir/ps.txt"
      fi
    fi
    sleep 0.2
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
    # 128 and above means a signal, and which signal is the diagnosis. 137 is
    # SIGKILL: something with the authority to send it decided this process should
    # die, and it was not this script.
    if [ "$rc" -ge 128 ]; then
      signal=$(kill -l "$((rc - 128))" 2>/dev/null || echo "signal $((rc - 128))")
      echo "exited after ${died_after}s with status $rc, which is SIG$signal" > "$dir/state"
    else
      echo "exited after ${died_after}s with status $rc (no signal)" > "$dir/state"
    fi
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
# The loader dies inside the first second with SIGKILL and no output, while
# /bin/echo and ./server/wineserver --version both run through the same harness and
# return cleanly. So it is not the harness and it is not Wine's code in general: it
# is this binary, killed at exec before dyld says anything. On Apple Silicon the
# candidate for a silent SIGKILL at exec is the code signature and the load commands
# the loader is linked with, which no other binary here uses:
#
#   -Wl,-segalign,0x1000,-pagezero_size,0x1000,-sectcreate,__TEXT,__info_plist,...
#
# So the signature, the segments and the embedded plist are read off the binary, and
# then the same binary is run again after an ad-hoc re-sign. If it runs after being
# signed, the cause is the signature and the fix belongs in the build rather than in
# Wine.
echo
echo "== signature of the loader =="
codesign -dvvv "$loader" 2>&1 | head -20 || echo "codesign could not read it"
if codesign --verify "$loader" 2>&1; then
  echo "codesign --verify: accepted"
else
  echo "codesign --verify: REJECTED (the message above says why)"
fi
if codesign --verify "$loader" > "$out/codesign.txt" 2>&1; then
  echo "codesign_verify=accepted" >> "$out/codesign.txt"
else
  echo "codesign_verify=rejected" >> "$out/codesign.txt"
fi
echo
echo "== segments and pagezero =="
otool -l "$loader" 2>&1 | grep -E 'segname|vmsize|fileoff|vmaddr' | head -16 || true
echo
echo "== embedded __TEXT,__info_plist, which only the loader has =="
if otool -s __TEXT __info_plist "$loader" > "$out/info_plist.txt" 2>&1 && [ -s "$out/info_plist.txt" ]; then
  head -6 "$out/info_plist.txt"
else
  echo "no __info_plist section found"
fi
echo
echo "== for comparison, the control that works =="
codesign -dvvv ./server/wineserver 2>&1 | head -8 || echo "codesign could not read wineserver"
otool -s __TEXT __info_plist ./server/wineserver 2>&1 | head -3 || true
echo
echo "== what macOS itself logged, which is where an exec-time kill is explained =="
log show --last 4m --style compact 2>/dev/null \
  | grep -iE 'wine|amfi|taskgated|code signature|not valid|killed' \
  | head -20 || echo "nothing usable from log show"

echo "== 1. a system binary, so a harness that loses output is ruled out =="
( unset WINEDLLPATH; unset WINEPREFIX; run_case control_echo 10 /bin/echo wine-control )

echo "== 2. another Wine binary from this same build, which is known to work =="
( unset WINEDLLPATH; unset WINEPREFIX; run_case control_wineserver 10 ./server/wineserver --version )

echo "== 3. the loader, relinked for a 16KB page machine =="
( unset WINEPREFIX; WINEDEBUG=-all WINEDLLPATH="$dllpath" run_case plain 30 "./$loader" --version )

echo "== 3b. the same source linked with no custom flags at all =="
( unset WINEPREFIX; WINEDEBUG=-all WINEDLLPATH="$dllpath" run_case noflags 30 ./loader/wine-noflags --version )

echo "== 4. the same loader with --help =="
( unset WINEPREFIX; WINEDEBUG=-all WINEDLLPATH="$dllpath" run_case help 30 "./$loader" --help )

echo
echo "== statuses =="
for case_dir in "$out"/control_echo "$out"/control_wineserver "$out"/plain "$out"/noflags "$out"/help; do
  [ -d "$case_dir" ] || continue
  printf '%-20s status=%-5s %s\n' "$(basename "$case_dir")" \
    "$(cat "$case_dir/status" 2>/dev/null || echo '?')" \
    "$(cat "$case_dir/state" 2>/dev/null)"
done

# One verdict, from the loader itself, whichever variant produced it.
for variant in plain noflags; do
  if [ "$(cat "$out/$variant/status" 2>/dev/null)" = "0" ]; then
    echo "verdict: the loader runs as $variant and printed: $(head -1 "$out/$variant/stdout" 2>/dev/null)" | tee "$out/verdict.txt"
    exit 0
  fi
done
echo "verdict: the loader does not run in either variant; see plain/log.txt and noflags/log.txt" | tee "$out/verdict.txt"
exit 1
