#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Two-way APK + app data + Android Keystore sync between Waydroid and Pocket 12L.

  pocket_sync.py plan                 # show what a sync would do (default)
  pocket_sync.py sync [--yes]         # do it
  pocket_sync.py sync --only de.hafas.android.sbahnberlin
  pocket_sync.py move org.thoughtcrime.securesms --to pocket

Per app, the side whose data changed since the last sync wins (if both did,
the newer data wins and a CONFLICT is logged; --conflict skip refuses).
APKs follow the newer versionName, but only when the APK set has native code
for the other side's ABI (Waydroid is x86_64, Pocket arm64); otherwise the
plan tells you which build to install by hand. Data never goes onto an older
app version. Keystore keys (software Keymaster blobs) are re-sealed for the
target's OS version/patch level and re-homed to the target app uid.

Exclusive apps (default: Signal) must never run on both sides: only the
enabled ("live") side is ever a source, and the copy on the other side stays
disabled. `move` hands the live copy over. microG (gms/gsf) and Play Store are
excluded by default because their data is a per-device identity.

Every overwritten target is backed up first under
~/.local/share/pocket-sync/backups/<time>/ (data tars + keystore DB).
"""
import argparse
import base64
import datetime
import hashlib
import json
import os
import random
import shlex
import shutil
import sqlite3
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pocket_sync_apk as apkmod  # noqa: E402
import pocket_sync_keyblob as kb  # noqa: E402

CACHE = Path.home() / '.cache/pocket-sync'
STATE = Path.home() / '.local/state/pocket-sync/state.json'
BACKUPS = Path.home() / '.local/share/pocket-sync/backups'
KEYSTORE_DB = '/data/misc/keystore/persistent.sqlite'
REMOTE_TMP = '/data/local/tmp/pocket-sync'
DEFAULT_EXCLUDE = {'com.google.android.gms', 'com.google.android.gsf', 'com.android.vending'}
DEFAULT_EXCLUSIVE = {'org.thoughtcrime.securesms'}
# Top-level entries of an app data dir that are per-device/ABI and never copied.
SKIP_RE = 'cache|code_cache|lib|lib-main|app_lib.*'
SQLITE_BUILDS = {'arm64-v8a': 'aarch64', 'x86_64': 'x86_64'}


class SyncError(RuntimeError):
    pass


def log(*a):
    print(*a, file=sys.stderr, flush=True)


# --------------------------------------------------------------------------- sides

class Side:
    """A guest reachable as root (sh) and through adb (pm/am)."""
    name = '?'

    def _root_argv(self, wrapped):
        raise NotImplementedError

    def _adb_argv(self, args):
        raise NotImplementedError

    def sh(self, script, *args, stdin=None, stdout=subprocess.PIPE, check=True):
        """Run a sh script as guest root. args become $1..$n. Returns stdout bytes."""
        full = 'set -- ' + ' '.join(shlex.quote(str(a)) for a in args) + '\n' + script
        enc = base64.b64encode(full.encode()).decode()
        wrapped = f'eval "$(echo {enc} | base64 -d)"'
        if len(wrapped) > 60000:
            raise SyncError('internal: guest script too long')
        p = subprocess.run(self._root_argv(wrapped), stdin=stdin if stdin is not None else subprocess.DEVNULL,
                           stdout=stdout, stderr=subprocess.PIPE)
        if check and p.returncode != 0:
            raise SyncError(f'{self.name}: guest script failed ({p.returncode}): '
                            + p.stderr.decode(errors='replace').strip()[-800:])
        return p.stdout if stdout is subprocess.PIPE else p.returncode

    def adb(self, *args, check=True):
        p = subprocess.run(self._adb_argv(args), stdin=subprocess.DEVNULL, capture_output=True)
        out = p.stdout.decode(errors='replace')
        if check and (p.returncode != 0 or 'Failure' in out or 'Exception' in out):
            raise SyncError(f'{self.name}: adb {" ".join(args[:3])} failed: '
                            + (out + p.stderr.decode(errors='replace')).strip()[-800:])
        return out

    def adb_shell(self, cmd, check=True):
        return self.adb('shell', cmd, check=check)

    def put_host_file(self, local, remote_name):
        """Copy a local file next to this side's adb client; return its path there."""
        raise NotImplementedError

    def prepare(self):
        pass


class Waydroid(Side):
    name = 'waydroid'

    def __init__(self, ssh_host, adb_serial):
        self.ssh, self.serial = ssh_host, adb_serial

    def _ssh(self, cmd):
        return ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10', '-o', 'ServerAliveInterval=15', self.ssh, cmd]

    def _root_argv(self, wrapped):
        return self._ssh('sudo -n waydroid shell -- sh -c ' + shlex.quote(wrapped))

    def _adb_argv(self, args):
        return self._ssh('adb -s ' + shlex.quote(self.serial) + ' ' + ' '.join(shlex.quote(a) for a in args))

    def prepare(self):
        subprocess.run(self._ssh('adb connect ' + shlex.quote(self.serial)), capture_output=True)

    def put_host_file(self, local, remote_name):
        dest = '.cache/pocket-sync/push/' + remote_name
        with open(local, 'rb') as f:
            subprocess.run(self._ssh('mkdir -p .cache/pocket-sync/push && cat > ' + shlex.quote(dest)),
                           stdin=f, check=True)
        return dest


class Pocket(Side):
    name = 'pocket'

    def __init__(self, ssh_target, ssh_port, pocketctl, adb_serial):
        self.ssh, self.port, self.pocketctl, self.serial = ssh_target, ssh_port, pocketctl, adb_serial

    def _ssh(self, cmd):
        port = ['-p', str(self.port)] if self.port else []
        return ['ssh', '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=10', '-o', 'ServerAliveInterval=15',
                *port, self.ssh, cmd]

    def _root_argv(self, wrapped):
        return self._ssh('python3 ' + self.pocketctl + ' exec -- /system/bin/sh -c ' + shlex.quote(wrapped))

    def prepare(self):
        # The guest adbd listens on the phone loopback; (re)attach the Termux adb client.
        if ':' in self.serial:
            subprocess.run(self._ssh('adb connect ' + shlex.quote(self.serial)), capture_output=True)

    def _adb_argv(self, args):
        return self._ssh('adb -s ' + shlex.quote(self.serial) + ' ' + ' '.join(shlex.quote(a) for a in args))

    def put_host_file(self, local, remote_name):
        dest = '.cache/pocket-sync/push/' + remote_name
        with open(local, 'rb') as f:
            subprocess.run(self._ssh('mkdir -p .cache/pocket-sync/push && cat > ' + shlex.quote(dest)),
                           stdin=f, check=True)
        return dest


# --------------------------------------------------------------------------- inventory

INVENTORY_SH = r'''
abis=$(getprop ro.product.cpu.abilist)
echo "ABI $abis"
for p in "$@"; do
  line=$(grep "^$p " /data/system/packages.list) || { echo "M $p"; continue; }
  set -- $line
  uid=$2 data=$4 vc=$8
  apkdir=$(ls -d /data/app/*/"$p"-* /data/app/"$p"-* 2>/dev/null | head -n 1)
  echo "P $p $uid $vc $data $apkdir"
  for f in "$apkdir"/*.apk; do
    [ -f "$f" ] && echo "A $p $(sha256sum "$f" | cut -d' ' -f1) $(stat -c %s "$f") ${f##*/}"
  done
  for kind in ce de; do
    if [ $kind = ce ]; then d=$data; else d=/data/user_de/0/$p; fi
    [ -d "$d" ] || continue
    echo "L $p $kind $(ls -Zd "$d" | cut -d' ' -f1) $d"
    cd "$d" || { echo "E $p cannot enter $d (CE storage locked?)"; continue; }
    find . \( -path ./cache -o -path ./code_cache -o -path ./lib -o -path ./lib-main -o -path './app_lib*' \) -prune \
      -o -type f -exec stat -c "F $p $kind %Y %s %n" {} +
  done
done
'''

APK_DIRS_SH = r'''
for d in /data/app/*/*/ /data/app/*/; do
  [ -f "$d/base.apk" ] || continue
  b=${d%/}; b=${b##*/}; echo "${b%-*}"
done
'''


class AppState:
    def __init__(self, pkg):
        self.pkg = pkg
        self.uid = self.version_code = self.data_dir = self.apk_dir = None
        self.apks = {}            # filename -> (sha, size)
        self.labels = {}          # ce/de -> (label, path)
        self.files = []           # (kind, mtime, size, name)
        self.errors = []
        self.enabled = True
        self.info = {}            # filename -> apk_info (host-side, filled lazily)

    @property
    def fingerprint(self):
        h = hashlib.sha256()
        for kind, mtime, size, name in sorted(self.files):
            h.update(f'{kind} {mtime} {size} {name}\n'.encode())
        return h.hexdigest()

    @property
    def newest(self):
        return max((m for _, m, _, _ in self.files), default=0)

    def version_name(self):
        base = self.info.get('base.apk')
        return base['version_name'] if base else None


def user_packages(side):
    try:
        out = side.adb_shell('pm list packages -3')
        pkgs = {l.split(':', 1)[1].strip() for l in out.splitlines() if l.startswith('package:')}
        disabled = side.adb_shell('pm list packages -d', check=False)
        off = {l.split(':', 1)[1].strip() for l in disabled.splitlines() if l.startswith('package:')}
        return pkgs, off
    except SyncError as e:
        log(f'{side.name}: adb package list failed ({e}); scanning /data/app instead')
        out = side.sh(APK_DIRS_SH).decode()
        return {l.strip() for l in out.splitlines() if l.strip()}, set()


def inventory(side, pkgs):
    out = side.sh(INVENTORY_SH, *sorted(pkgs)).decode(errors='replace')
    abis, apps = [], {}
    for line in out.splitlines():
        tag, _, rest = line.partition(' ')
        if tag == 'ABI':
            abis = [a for a in rest.split(',') if a]
            continue
        pkg, _, rest = rest.partition(' ')
        if tag == 'M':
            continue
        app = apps.setdefault(pkg, AppState(pkg))
        if tag == 'P':
            f = rest.split(' ')
            app.uid, app.version_code, app.data_dir = int(f[0]), int(f[1]), f[2]
            app.apk_dir = f[3] if len(f) > 3 and f[3] else None
        elif tag == 'A':
            sha, size, name = rest.split(' ', 2)
            app.apks[name] = (sha, int(size))
        elif tag == 'L':
            kind, label, path = rest.split(' ', 2)
            app.labels[kind] = (label, path)
        elif tag == 'F':
            kind, mtime, size, name = rest.split(' ', 3)
            app.files.append((kind, int(mtime), int(size), name))
        elif tag == 'E':
            app.errors.append(rest)
    return abis, apps


def fetch_apks(side, app):
    """Pull the app's APKs into the host cache (by sha256) and parse them."""
    store = CACHE / 'apks'
    store.mkdir(parents=True, exist_ok=True)
    for name, (sha, size) in app.apks.items():
        path = store / f'{sha}.apk'
        if not path.exists():
            tmp = path.with_suffix('.part')
            with open(tmp, 'wb') as f:
                side.sh('cat "$1"', f'{app.apk_dir}/{name}', stdout=f)
            if hashlib.sha256(tmp.read_bytes()).hexdigest() != sha:
                tmp.unlink()
                raise SyncError(f'{side.name}: {app.pkg}/{name} changed while copying')
            tmp.replace(path)
        app.info[name] = apkmod.apk_info(path)
        app.info[name]['path'] = path


# --------------------------------------------------------------------------- data copy

EXPORT_SH = r'''
cd "$1" || exit 3
IFS='
'
set -- $(ls -A | grep -v -x -E "$2")
[ $# -gt 0 ] || exit 0
exec tar -cf - "$@"
'''

IMPORT_SH = r'''
dir=$1 uid=$2 skip=$3 stage=$4
rm -rf "$stage" && mkdir -p "$stage" && cd "$stage" || exit 4
tar -xf - || exit 5
label=$(ls -Zd "$dir" | cut -d' ' -f1)
cd "$dir" || exit 6
ls -A | grep -v -x -E "$skip" | while IFS= read -r e; do rm -rf "./$e" || exit 7; done
cd "$stage" || exit 8
ls -A | while IFS= read -r e; do
  # cp -a can fail to keep attributes on FUSE/AppFuse CE storage; fall back to -R.
  cp -a "./$e" "$dir/" 2>/dev/null || cp -R "./$e" "$dir/" || exit 9
  chown -R -h "$uid:$uid" "$dir/$e" || exit 10
  # Pocket's guest root may not relabel (host app domain); new files inherit the dir label
  # there, so only fail if something actually ended up with a different label.
  case $label in *:*)
    chcon -R -h "$label" "$dir/$e" 2>/dev/null ||
      ! find "$dir/$e" -exec ls -Zd {} + | cut -d' ' -f1 | grep -q -v -x -F "$label" || exit 11;;
  esac
done || exit 12
cd / && rm -rf "$stage"
'''


def export_data(side, app, kind, dest):
    label, path = app.labels[kind]
    with open(dest, 'wb') as f:
        side.sh(EXPORT_SH, path, SKIP_RE, stdout=f)


def import_data(side, app, kind, tar_path):
    label, path = app.labels[kind]
    if os.path.getsize(tar_path) == 0:
        # Empty source dir: just clear the target.
        side.sh('cd "$1" && ls -A | grep -v -x -E "$2" | while IFS= read -r e; do rm -rf "./$e"; done',
                path, SKIP_RE)
        return
    with open(tar_path, 'rb') as f:
        side.sh(IMPORT_SH, path, app.uid, SKIP_RE, f'{REMOTE_TMP}/stage-{app.pkg}-{kind}', stdin=f)


# --------------------------------------------------------------------------- keystore

def keystore_copy(side):
    script = r'''
db=$1 out=$2/ks-copy.sqlite
mkdir -p "$2"
if command -v sqlite3 >/dev/null 2>&1; then
  rm -f "$out"; sqlite3 "$db" ".backup '$out'" && cat "$out"; rc=$?; rm -f "$out"; exit $rc
fi
[ -e "$db-wal" ] && { echo "keystore DB is in WAL mode and the guest has no sqlite3" >&2; exit 9; }
cat "$db"
'''
    raw = side.sh(script, KEYSTORE_DB, REMOTE_TMP)
    db = sqlite3.connect(':memory:')
    db.deserialize(raw)
    return db, raw


def build_sqlite(arch, binary):
    """Cross-build a static sqlite3 shell with zig (for guests without one)."""
    import re
    import urllib.request
    import zipfile
    work = binary.parent
    work.mkdir(parents=True, exist_ok=True)
    src = sorted(work.glob('sqlite-amalgamation-*/sqlite3.c'))
    if not src:
        page = urllib.request.urlopen('https://www.sqlite.org/download.html', timeout=30).read().decode()
        rel = re.search(r'20\d\d/sqlite-amalgamation-\d+\.zip', page)
        if not rel:
            raise SyncError('could not find the sqlite amalgamation download')
        archive = work / 'amalgamation.zip'
        archive.write_bytes(urllib.request.urlopen('https://www.sqlite.org/' + rel.group(0), timeout=120).read())
        zipfile.ZipFile(archive).extractall(work)
        src = sorted(work.glob('sqlite-amalgamation-*/sqlite3.c'))
    d = src[-1].parent
    log(f'building static sqlite3 for {arch} with zig')
    subprocess.run(['zig', 'cc', '-target', f'{arch}-linux-musl', '-static', '-O2', '-s',
                    '-DSQLITE_OMIT_LOAD_EXTENSION', '-DSQLITE_THREADSAFE=0',
                    '-o', str(binary), str(d / 'shell.c'), str(d / 'sqlite3.c')], check=True)


def sqlite_on(side, abis):
    """Path of a sqlite3 binary usable on the guest (pushes a static build if needed)."""
    have = side.sh('command -v sqlite3 || true').decode().strip()
    if have:
        return have
    for abi in abis:
        arch = SQLITE_BUILDS.get(abi)
        if not arch:
            continue
        binary = CACHE / f'sqlite/sqlite3-{arch}'
        if not binary.exists():
            build_sqlite(arch, binary)
        dest = f'{REMOTE_TMP}/sqlite3'
        sha = hashlib.sha256(binary.read_bytes()).hexdigest()
        present = side.sh('sha256sum "$1" 2>/dev/null | cut -d" " -f1', dest).decode().strip()
        if present != sha:
            with open(binary, 'rb') as f:
                side.sh('mkdir -p "${1%/*}" && cat > "$1" && chmod 700 "$1"', dest, stdin=f)
        return dest
    raise SyncError(f'{side.name}: no sqlite3 for ABIs {abis}')


def _lit(v):
    if v is None:
        return 'NULL'
    if isinstance(v, bool):
        return str(int(v))
    if isinstance(v, int):
        return str(v)
    if isinstance(v, float):
        return repr(v)
    if isinstance(v, (bytes, bytearray, memoryview)):
        return "X'" + bytes(v).hex() + "'"
    return "'" + str(v).replace("'", "''") + "'"


def _latest_blobs(db, key_id):
    rows = db.execute('SELECT id, subcomponent_type, blob FROM blobentry WHERE keyentryid=? ORDER BY id',
                      (key_id,)).fetchall()
    latest = {}
    for bid, st, blob in rows:
        latest[st] = (bid, blob)
    return latest


def target_reference(db):
    """Version tags (with enforcement list + keyparameter rows) of the target's newest key."""
    best = None
    for (key_id,) in db.execute('SELECT id FROM keyentry WHERE state=1'):
        blob = _latest_blobs(db, key_id).get(0)
        if not blob:
            continue
        try:
            k = kb.KeyBlob(blob[1])
        except kb.BlobError:
            continue
        v = k.versions()
        rank = (v.get(kb.TAG_OS_PATCHLEVEL, 0), v.get(kb.TAG_OS_VERSION, 0))
        if best is None or rank > best[0]:
            best = (rank, key_id, k)
    if best is None:
        return None
    _, key_id, k = best
    tags = ','.join(str(t) for t in kb.VERSION_TAGS)
    params = db.execute(f'SELECT tag, data, security_level FROM keyparameter WHERE keyentryid=? AND tag IN ({tags})',
                        (key_id,)).fetchall()
    uuid = db.execute('SELECT km_uuid FROM keyentry WHERE id=?', (key_id,)).fetchone()[0]
    return {'hw': [(t, v) for t, v in k.hw if t in kb.VERSION_TAGS],
            'sw': [(t, v) for t, v in k.sw if t in kb.VERSION_TAGS],
            'params': params, 'uuid': uuid}


def keystore_sql(src_db, src_uid, tgt_db, tgt_uid):
    """SQL replacing tgt_uid's app keys with src_uid's. Returns (sql, moved, skipped)."""
    ref = target_reference(tgt_db)
    keys = src_db.execute('SELECT id, alias FROM keyentry WHERE domain=0 AND namespace=? AND state=1 AND key_type=0',
                          (src_uid,)).fetchall()
    moved, skipped, out = [], [], []
    sub = f'SELECT id FROM keyentry WHERE domain=0 AND namespace={tgt_uid}'
    out += ['BEGIN IMMEDIATE;',
            f'DELETE FROM blobmetadata WHERE blobentryid IN (SELECT id FROM blobentry WHERE keyentryid IN ({sub}));',
            f'DELETE FROM blobentry WHERE keyentryid IN ({sub});',
            f'DELETE FROM keyparameter WHERE keyentryid IN ({sub});',
            f'DELETE FROM keymetadata WHERE keyentryid IN ({sub});',
            f'DELETE FROM grant WHERE keyentryid IN ({sub});',
            f'DELETE FROM keyentry WHERE domain=0 AND namespace={tgt_uid};']
    used_ids = {r[0] for r in tgt_db.execute('SELECT id FROM keyentry')}
    for key_id, alias in keys:
        blobs = _latest_blobs(src_db, key_id)
        if 0 not in blobs:
            skipped.append((alias, 'no key blob'))
            continue
        meta = {bid: src_db.execute('SELECT tag, data FROM blobmetadata WHERE blobentryid=?', (bid,)).fetchall()
                for bid, _ in blobs.values()}
        if any(tag == 0 for tag, _ in meta[blobs[0][0]]):
            skipped.append((alias, 'super-encrypted (bound to lock screen/auth); re-create it in the app'))
            continue
        try:
            k = kb.KeyBlob(blobs[0][1])
        except kb.BlobError as e:
            skipped.append((alias, str(e)))
            continue
        if ref is None:
            skipped.append((alias, 'target keystore has no reference key to copy OS/patch versions from'))
            continue
        k.hw = [(t, v) for t, v in k.hw if t not in kb.VERSION_TAGS] + ref['hw']
        k.sw = [(t, v) for t, v in k.sw if t not in kb.VERSION_TAGS] + ref['sw']
        new_blob = k.serialize()
        new_id = random.randrange(1, 2**63)
        while new_id in used_ids:
            new_id = random.randrange(1, 2**63)
        used_ids.add(new_id)
        out.append(f'INSERT INTO keyentry (id, key_type, domain, namespace, alias, state, km_uuid) '
                   f'VALUES ({new_id}, 0, 0, {tgt_uid}, {_lit(alias)}, 1, {_lit(ref["uuid"])});')
        for st, (bid, blob) in sorted(blobs.items()):
            out.append(f'INSERT INTO blobentry (subcomponent_type, keyentryid, blob) '
                       f'VALUES ({st}, {new_id}, {_lit(new_blob if st == 0 else blob)});')
            for tag, data in meta[bid]:
                if tag == 4:  # KmUuid -> the target's KeyMint instance
                    data = ref['uuid']
                out.append(f'INSERT INTO blobmetadata (blobentryid, tag, data) VALUES ((SELECT max(id) FROM blobentry '
                           f'WHERE keyentryid={new_id} AND subcomponent_type={st}), {tag}, {_lit(data)});')
        for tag, data, level in src_db.execute('SELECT tag, data, security_level FROM keyparameter WHERE keyentryid=?',
                                               (key_id,)):
            if tag in kb.VERSION_TAGS:
                continue
            out.append(f'INSERT INTO keyparameter (keyentryid, tag, data, security_level) '
                       f'VALUES ({new_id}, {tag}, {_lit(data)}, {level});')
        for tag, data, level in ref['params']:
            out.append(f'INSERT INTO keyparameter (keyentryid, tag, data, security_level) '
                       f'VALUES ({new_id}, {tag}, {_lit(data)}, {level});')
        for tag, data in src_db.execute('SELECT tag, data FROM keymetadata WHERE keyentryid=?', (key_id,)):
            out.append(f'INSERT INTO keymetadata (keyentryid, tag, data) VALUES ({new_id}, {tag}, {_lit(data)});')
        moved.append(alias)
    out.append('COMMIT;')
    return '\n'.join(out) + '\n', moved, skipped


def apply_keystore_sql(side, abis, sql, backup_dir):
    (backup_dir / f'{side.name}-keystore.sql').write_text(sql)
    binary = sqlite_on(side, abis)
    path = backup_dir / f'{side.name}-keystore.sql'
    with open(path, 'rb') as f:
        side.sh('"$1" "$2"', binary, KEYSTORE_DB, stdin=f)


# --------------------------------------------------------------------------- planning

class Plan:
    def __init__(self, pkg):
        self.pkg = pkg
        self.src = self.dst = None      # data direction (Side names)
        self.install = None             # (side name, files, from side name)
        self.notes = []
        self.blocked = None
        self.conflict = False
        self.exclusive = False
        self.move = False


def compare_versions(a, b):
    """-1/0/1 comparing AppStates by versionName then versionCode."""
    ka, kb_ = apkmod.version_key(a.version_name()), apkmod.version_key(b.version_name())
    if ka != kb_:
        return -1 if ka < kb_ else 1
    return 0


def make_plan(pkg, sides, apps, abis, state, args):
    w, p = sides
    plan = Plan(pkg)
    plan.exclusive = pkg in args.exclusive
    a, b = apps[w.name].get(pkg), apps[p.name].get(pkg)
    for x in (a, b):
        if x and x.errors:
            plan.blocked = '; '.join(x.errors)
            return plan
    by = {w.name: a, p.name: b}

    # Data direction.
    forced = getattr(args, 'from_side', None)
    if forced:
        other = p.name if forced == w.name else w.name
        if by[forced] is None:
            plan.blocked = f'not installed on {forced}'
            return plan
        if plan.exclusive and (not by[forced].enabled or (by[other] and by[other].enabled)):
            plan.blocked = f'exclusive app: --from needs it enabled only on {forced}; use `move` instead'
            return plan
        plan.src, plan.dst = forced, other
        if by[other] is not None and by[forced].fingerprint == by[other].fingerprint:
            plan.src = plan.dst = None
    elif args.move_to:
        plan.move = True
        plan.dst = args.move_to
        plan.src = w.name if args.move_to == p.name else p.name
        if by[plan.src] is None:
            plan.blocked = f'not installed on {plan.src}'
            return plan
    elif a is None or b is None:
        plan.src = w.name if a else p.name
        plan.dst = p.name if a else w.name
    elif plan.exclusive:
        live = [s for s in (w.name, p.name) if by[s].enabled]
        if len(live) != 1:
            plan.blocked = ('exclusive app is enabled on both sides; disable the stale one or use '
                            f'`move {pkg} --to <side>`' if live else 'exclusive app is disabled on both sides')
            return plan
        plan.src = live[0]
        plan.dst = p.name if plan.src == w.name else w.name
        if by[plan.src].fingerprint == by[plan.dst].fingerprint:
            plan.src = plan.dst = None
    else:
        fa, fb = a.fingerprint, b.fingerprint
        last = state.get(pkg)
        if fa == fb:
            pass
        elif last and fa == last and fb != last:
            plan.src, plan.dst = p.name, w.name
        elif last and fb == last and fa != last:
            plan.src, plan.dst = w.name, p.name
        else:
            plan.conflict = bool(last)
            if plan.conflict and args.conflict == 'skip':
                plan.blocked = 'CONFLICT: data changed on both sides since the last sync (--conflict newest to force)'
                return plan
            plan.src, plan.dst = (w.name, p.name) if a.newest >= b.newest else (p.name, w.name)
            if not last:
                plan.notes.append('first sync: newest data wins')
            else:
                plan.notes.append('CONFLICT: both changed; newest data wins, the other side is backed up')

    # APK: carry the newer version to the other side when it can run there.
    if a and b:
        cmp = compare_versions(a, b)
        newer, older = (a, b) if cmp > 0 else (b, a) if cmp < 0 else (None, None)
    else:
        newer, older = (a or b), None
    if newer is not None and forced and (p.name if newer is a else w.name) == forced:
        plan.notes.append(f'{forced} would get the newer {newer.version_name()} APK; not touched in a --from run')
        newer = None
    if newer is not None:
        to = p.name if newer is a else w.name
        try:
            files, abi = apkmod.select_for_abis(newer.info, abis[to])
            plan.install = (to, files, w.name if newer is a else p.name)
        except ValueError as e:
            what = 'install' if older is None else f'update to {newer.version_name()}'
            plan.notes.append(f'{to}: {what} by hand ({e})')
    # Data must not land on an older app than produced it.
    if plan.src:
        s, d = by[plan.src], by[plan.dst]
        will_install = plan.install and plan.install[0] == plan.dst
        if d is None and not will_install:
            plan.blocked = f'not installed on {plan.dst} and its APK cannot be copied there'
        elif d is not None and not will_install and compare_versions(d, s) < 0:
            plan.blocked = (f'{plan.dst} has older {d.version_name()} than {plan.src} {s.version_name()}; '
                            'update it there first')
    return plan


def describe(plan, apps):
    bits = []
    if plan.install:
        to, files, frm = plan.install
        bits.append(f'install {"+".join(files)} {frm}->{to}')
    if plan.src:
        src = apps[plan.src].get(plan.pkg)
        bits.append(f'data+keys {plan.src}->{plan.dst} ({len(src.files)} files, '
                    f'{sum(f[2] for f in src.files) // 1024} KiB)')
    if plan.exclusive and plan.src:
        bits.append(f'keep {plan.dst} copy disabled' if not plan.move else f'enable on {plan.dst}, disable on {plan.src}')
    if not bits and not plan.blocked:
        bits.append('in sync')
    if plan.blocked:
        bits = ['BLOCKED: ' + plan.blocked]
    return f'{plan.pkg}: ' + '; '.join(bits) + ''.join(f'\n    note: {n}' for n in plan.notes)


# --------------------------------------------------------------------------- execution

def execute(plan, sides, apps, abis, backup_dir):
    by_name = {s.name: s for s in sides}
    pkg = plan.pkg
    for s in sides:
        if apps[s.name].get(pkg):
            s.adb_shell(f'am force-stop {pkg}', check=False)

    if plan.install:
        to, files, frm = plan.install
        dst_side, src_app = by_name[to], apps[frm][pkg]
        remote = [dst_side.put_host_file(src_app.info[f]['path'], f'{pkg}-{f}') for f in files]
        log(f'  installing {pkg} on {to}')
        dst_side.adb('install-multiple', '-r', *remote)
        _, fresh = inventory(dst_side, {pkg})
        apps[to][pkg] = fresh[pkg]
        fetch_apks(dst_side, fresh[pkg])
        dst_side.adb_shell(f'am force-stop {pkg}', check=False)

    if not plan.src:
        return
    src_side, dst_side = by_name[plan.src], by_name[plan.dst]
    s, d = apps[plan.src][pkg], apps[plan.dst][pkg]
    if s.uid is None or d.uid is None:
        raise SyncError('uid unknown after install')

    # Keystore first: if it fails, nothing of the target's data has been touched.
    src_db, _ = keystore_copy(src_side)
    tgt_db, tgt_raw = keystore_copy(dst_side)
    (backup_dir / f'{plan.dst}-keystore.sqlite').write_bytes(tgt_raw)
    (backup_dir / f'{plan.dst}-{pkg}.json').write_text(json.dumps({'uid': d.uid}))
    sql, moved, skipped = keystore_sql(src_db, s.uid, tgt_db, d.uid)
    for alias, why in skipped:
        log(f'  key {alias!r} not moved: {why}')
    has_target_keys = tgt_db.execute('SELECT count(*) FROM keyentry WHERE domain=0 AND namespace=?',
                                     (d.uid,)).fetchone()[0]
    if moved or has_target_keys:
        apply_keystore_sql(dst_side, abis[plan.dst], sql, backup_dir)
        log(f'  keys moved: {", ".join(map(str, moved)) or "none"} (target had {has_target_keys})')

    for kind in ('ce', 'de'):
        if kind not in s.labels or kind not in d.labels:
            continue
        src_tar = backup_dir / f'{plan.src}-{pkg}-{kind}.tar'
        dst_tar = backup_dir / f'{plan.dst}-{pkg}-{kind}.before.tar'
        export_data(dst_side, d, kind, dst_tar)
        export_data(src_side, s, kind, src_tar)
        import_data(dst_side, d, kind, src_tar)
        log(f'  {kind}: {src_tar.stat().st_size // 1024} KiB copied')

    if plan.exclusive:
        live, stale = (dst_side, src_side) if plan.move else (src_side, dst_side)
        live.adb_shell(f'pm enable {pkg}', check=False)
        stale.adb_shell(f'pm disable-user --user 0 {pkg}')


def restore(side, pkg, backup_dir=None):
    """Put back the data and keys `side` had for `pkg` before a sync overwrote them."""
    if backup_dir is None:
        runs = sorted((p for p in BACKUPS.glob('*') if (p / f'{side.name}-{pkg}.json').exists()), reverse=True)
        if not runs:
            raise SyncError(f'no backup of {pkg} on {side.name} under {BACKUPS}')
        backup_dir = runs[0]
    meta = json.loads((backup_dir / f'{side.name}-{pkg}.json').read_text())
    log(f'restoring {pkg} on {side.name} from {backup_dir}')
    side.prepare()
    side.adb_shell(f'am force-stop {pkg}', check=False)
    abis, apps = inventory(side, {pkg})
    if pkg not in apps:
        raise SyncError(f'{pkg} is not installed on {side.name}; install it first')
    app = apps[pkg]
    old_db = sqlite3.connect(':memory:')
    old_db.deserialize((backup_dir / f'{side.name}-keystore.sqlite').read_bytes())
    cur_db, cur_raw = keystore_copy(side)
    undo = BACKUPS / (datetime.datetime.now().strftime('%Y%m%d-%H%M%S') + '-before-restore')
    undo.mkdir(parents=True, exist_ok=True)
    (undo / f'{side.name}-keystore.sqlite').write_bytes(cur_raw)
    sql, moved, skipped = keystore_sql(old_db, meta['uid'], cur_db, app.uid)
    apply_keystore_sql(side, abis, sql, undo)
    log(f'  keys restored: {", ".join(map(str, moved)) or "none"}' + ''.join(f'\n  key {a!r} skipped: {w}' for a, w in skipped))
    for kind in ('ce', 'de'):
        tar = backup_dir / f'{side.name}-{pkg}-{kind}.before.tar'
        if tar.exists() and kind in app.labels:
            export_data(side, app, kind, undo / f'{side.name}-{pkg}-{kind}.before.tar')
            import_data(side, app, kind, tar)
            log(f'  {kind}: restored {tar.stat().st_size // 1024} KiB')
    (undo / f'{side.name}-{pkg}.json').write_text(json.dumps({'uid': app.uid}))
    # Forget the recorded sync point so the next plan re-decides this app.
    state = load_state()
    state.pop(pkg, None)
    save_state(state)
    log(f'done; the state before this restore is in {undo}')
    return 0


# --------------------------------------------------------------------------- main

def load_state():
    try:
        return json.loads(STATE.read_text())
    except (OSError, ValueError):
        return {}


def save_state(state):
    STATE.parent.mkdir(parents=True, exist_ok=True)
    tmp = STATE.with_suffix('.tmp')
    tmp.write_text(json.dumps(state, indent=1, sort_keys=True))
    tmp.replace(STATE)


def prune_backups(keep):
    if not BACKUPS.exists():
        return
    runs = sorted(p for p in BACKUPS.iterdir() if p.is_dir())
    for old in runs[:-keep] if keep > 0 else []:
        shutil.rmtree(old, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('command', nargs='?', default='plan', choices=['plan', 'sync', 'move', 'restore'])
    ap.add_argument('package', nargs='?', help='package for `move` / `restore`')
    ap.add_argument('--to', dest='move_to', choices=['waydroid', 'pocket'],
                    help='target side for `move`; side to roll back for `restore`')
    ap.add_argument('--backup', type=Path, help='backup run dir for `restore` (default: newest with that app)')
    ap.add_argument('--only', action='append', default=[], help='limit to these packages (repeatable)')
    ap.add_argument('--exclude', action='append', default=[], help='skip these packages (repeatable)')
    ap.add_argument('--include', action='append', default=[], help='un-exclude a default-excluded package')
    ap.add_argument('--exclusive', action='append', default=[],
                    help='extra apps that must only be live on one side (default: Signal)')
    ap.add_argument('--conflict', choices=['newest', 'skip'], default='newest')
    ap.add_argument('--yes', '-y', action='store_true', help='do not ask before syncing')
    ap.add_argument('--keep-backups', type=int, default=10)
    ap.add_argument('--waydroid-ssh', default='localhost')
    ap.add_argument('--waydroid-adb', default='192.168.240.112:5555')
    ap.add_argument('--from', dest='from_side', choices=['waydroid', 'pocket'],
                    help='one-way run: copy from this side, only change the other one')
    ap.add_argument('--phone', default='pf-phone2', help='Termux ssh target (host alias or user@host)')
    ap.add_argument('--phone-port', type=int, default=None, help='ssh port (default: from ssh config)')
    ap.add_argument('--pocketctl', default='~/pocketctl.py', help='pocketctl.py path on the phone')
    ap.add_argument('--pocket-adb', default='127.0.0.1:5555')
    args = ap.parse_args()
    args.exclusive = DEFAULT_EXCLUSIVE | set(args.exclusive)
    exclude = (DEFAULT_EXCLUDE - set(args.include)) | set(args.exclude)
    sides = (Waydroid(args.waydroid_ssh, args.waydroid_adb),
             Pocket(args.phone, args.phone_port, args.pocketctl, args.pocket_adb))
    if args.command == 'restore':
        if not args.package or not args.move_to:
            ap.error('restore needs a package and --to waydroid|pocket')
        try:
            return restore(next(s for s in sides if s.name == args.move_to), args.package, args.backup)
        except (SyncError, OSError) as e:
            log('error:', e)
            return 2
    if args.command == 'move':
        if not args.package or not args.move_to:
            ap.error('move needs a package and --to waydroid|pocket')
        args.only = [args.package]
        args.exclusive.add(args.package)
    else:
        args.move_to = None

    apps, abis = {}, {}
    try:
        for side in sides:
            side.prepare()
            pkgs, disabled = user_packages(side)
            pkgs = {x for x in pkgs if x not in exclude and (not args.only or x in args.only)}
            abis[side.name], apps[side.name] = inventory(side, pkgs)
            for pkg, app in apps[side.name].items():
                app.enabled = pkg not in disabled
                fetch_apks(side, app)
            log(f'{side.name}: {len(apps[side.name])} apps, ABIs {",".join(abis[side.name])}')
    except (SyncError, OSError) as e:
        log('error:', e)
        return 2

    state = load_state()
    plans = [make_plan(pkg, sides, apps, abis, state, args)
             for pkg in sorted(set(apps['waydroid']) | set(apps['pocket']))]
    for plan in plans:
        print(describe(plan, apps))
    todo = [pl for pl in plans if not pl.blocked and (pl.install or pl.src)]
    if args.command == 'plan' or not todo:
        return 0
    if not args.yes:
        if input(f'Sync {len(todo)} app(s)? [y/N] ').strip().lower() != 'y':
            return 1
    backup_dir = BACKUPS / datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    backup_dir.mkdir(parents=True, exist_ok=True)
    failed = 0
    for plan in todo:
        log(f'{plan.pkg}:')
        try:
            execute(plan, sides, apps, abis, backup_dir)
            # Record what both sides look like now, for next run's change detection.
            after = {}
            for side in sides:
                _, fresh = inventory(side, {plan.pkg})
                if plan.pkg in fresh:
                    after[side.name] = fresh[plan.pkg].fingerprint
            if len(set(after.values())) == 1:
                state[plan.pkg] = next(iter(after.values()))
            else:
                log(f'  warning: fingerprints differ after copy ({after}); next run will re-check')
                state[plan.pkg] = after.get(plan.dst)
            save_state(state)
        except (SyncError, OSError) as e:
            failed += 1
            log(f'  FAILED: {e}\n  backups in {backup_dir}')
    prune_backups(args.keep_backups)
    log(f'backups: {backup_dir}')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
