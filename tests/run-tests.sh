#!/usr/bin/env bash
# Parser/verdict regression tests for twsu-check (offline, no root needed).
set -u
cd "$(dirname "$0")"
CHECK=../bin/twsu-check
fail=0

expect() { # fixture expected_verdict [python_assertion...]
    local fixture=$1 want=$2; shift 2
    local out
    out=$(python3 "$CHECK" --parse-file "fixtures/$fixture" --offline) || { echo "FAIL $fixture: checker crashed"; fail=1; return; }
    local got
    got=$(python3 -c "import json,sys; print(json.loads(sys.argv[1])['verdict'])" "$out")
    if [[ "$got" != "$want" ]]; then
        echo "FAIL $fixture: verdict=$got, expected $want"; fail=1; return
    fi
    for assertion in "$@"; do
        if ! python3 -c '
import json,sys
st=json.loads(sys.argv[1])
assert eval(sys.argv[2]), "assertion failed: " + sys.argv[2]
' "$out" "$assertion"; then
            echo "FAIL $fixture: $assertion"; fail=1; return
        fi
    done
    echo "PASS $fixture → $got"
}

expect nothing.txt          UP_TO_DATE
expect clean-upgrade.txt    SAFE \
    "st['counts']['upgrade']==213" \
    "st['counts']['new']==4" \
    "st['download_size']=='316.1 MiB'" \
    "'kernel-default-6.15.4-1.1' in st['packages']['new']" \
    "st['counts']['reboot']==6" \
    "any('20260625-0 -> 20260630-0' in p for p in st['packages']['product'])"
expect problems.txt         UNSAFE \
    "len(st['problems'])==2" \
    "len(st['problems'][0]['solutions'])==2"
expect vendor-change.txt    UNSAFE \
    "st['counts']['vendor_change']==5" \
    "any('LEAVE the Packman vendor' in r['text'] for r in st['reasons'])"
expect held-back.txt        CAUTION \
    "st['counts']['held_back']==8" \
    "any('Packman likely not in sync' in r['text'] for r in st['reasons'])"
expect downgrade-remove.txt CAUTION \
    "st['counts']['downgrade']==2" "st['counts']['remove']==3"
expect critical-removal.txt UNSAFE \
    "any('REMOVED' in r['text'] and 'ffmpeg-7' in r['text'] for r in st['reasons'])" \
    "st['counts']['remove']==2"

# --- unit checks on the classification regexes (regression for audit fixes) ---
if python3 - "$CHECK" <<'PY'
import sys
from importlib.machinery import SourceFileLoader
m = SourceFileLoader('twsu', sys.argv[1]).load_module()
# CRITICAL_RE must anchor to whole package names, not prefixes
for n in ['grub2-branding-openSUSE', 'systemd-experimental', 'rpm-build',
          'kernel-default-devel', 'pipewire-aptx', 'mesa-vulkan-drivers']:
    assert not m.CRITICAL_RE.match(n), 'false critical: ' + n
for n in ['kernel-default', 'kernel-default-6.15.4', 'systemd', 'glibc',
          'ffmpeg-7', 'grub2', 'Mesa', 'libvulkan1', 'zypper']:
    assert m.CRITICAL_RE.match(n), 'missed critical: ' + n
# SOL_INSTALL_RE must ignore "do not install" rejections
assert not m.SOL_INSTALL_RE.search('Solution 1: do not install mpv-0.40.0-1.x86_64')
assert m.SOL_INSTALL_RE.search('Solution 1: install polaris-10.2.0-1.1.x86_64 from vendor openSUSE')
# download-size parser accepts both wordings
assert m.SIZE_RE.search('Package download size: 316.1 MiB')
assert m.SIZE_RE.search('Overall download size: 316.1 MiB')
PY
then echo "PASS regex unit checks"; else echo "FAIL regex unit checks"; fail=1; fi

exit $fail
