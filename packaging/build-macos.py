
import os, shutil, subprocess, sys, tarfile

ROOT = "/Users/bellgrell/Documents/migrator-mmv"
VER = "0.9.0"
OUT = os.path.expanduser("~/Documents/migrator-release/macos")
STAGE = os.path.join(OUT, "mini-migrator-" + VER + "-macos-arm64")
BIN = os.path.join(ROOT, "build", "migrator")

def otool_deps(path):
    try:
        out = subprocess.check_output(["otool", "-L", path], text=True)
    except subprocess.CalledProcessError:
        return []
    deps = []
    for line in out.splitlines()[1:]:
        line = line.strip()
        if not line:
            continue
        deps.append(line.split(" (")[0].strip())
    return deps

def is_system(p):
    return p.startswith("/usr/lib/") or p.startswith("/System/")

if os.path.isdir(STAGE):
    shutil.rmtree(STAGE)
os.makedirs(os.path.join(STAGE, "bin"))
os.makedirs(os.path.join(STAGE, "lib"))

shutil.copy2(BIN, os.path.join(STAGE, "bin", "migrator"))
for d in ("config", "web"):
    src = os.path.join(ROOT, d)
    if os.path.isdir(src):
        shutil.copytree(src, os.path.join(STAGE, d))
for f in ("README.md", "README.zh.md", "LICENSE"):
    src = os.path.join(ROOT, f)
    if os.path.exists(src):
        shutil.copy2(src, os.path.join(STAGE, f))

copied = {}
queue = [os.path.join(STAGE, "bin", "migrator")]
seen = set()

while queue:
    target = queue.pop()
    if target in seen:
        continue
    seen.add(target)
    for dep in otool_deps(target):
        if is_system(dep):
            continue
        base = os.path.basename(dep)
        staged = os.path.join(STAGE, "lib", base)
        if dep not in copied:
            if not os.path.exists(dep):
                print("missing dep", dep)
                continue
            shutil.copy2(dep, staged)
            os.chmod(staged, 0o755)
            copied[dep] = staged
            queue.append(staged)
        subprocess.run(["install_name_tool", "-change", dep,
                        "@executable_path/../lib/" + base, target], check=False)

for dep, staged in copied.items():
    subprocess.run(["install_name_tool", "-id",
                    "@executable_path/../lib/" + os.path.basename(staged), staged],
                   check=False)

for root, dirs, files in os.walk(STAGE):
    for name in files:
        fp = os.path.join(root, name)
        if fp.endswith((".dylib", "migrator")):
            subprocess.run(["codesign", "--force", "--sign", "-", fp],
                           check=False, capture_output=True)

runsh = os.path.join(STAGE, "run.sh")
with open(runsh, "w") as f:
    f.write("#!/bin/sh\n")
    f.write('DIR=$(cd "$(dirname "$0")" && pwd)\n')
    f.write('export DYLD_LIBRARY_PATH="$DIR/lib:$DYLD_LIBRARY_PATH"\n')
    f.write('exec "$DIR/bin/migrator" "$@"\n')
os.chmod(runsh, 0o755)

tarball = os.path.join(OUT, "mini-migrator-" + VER + "-macos-arm64.tar.gz")
with tarfile.open(tarball, "w:gz") as t:
    t.add(STAGE, arcname=os.path.basename(STAGE))

print("staged:", STAGE)
print("libs:", len(copied))
print("tarball:", tarball, os.path.getsize(tarball) // (1024 * 1024), "MB")
