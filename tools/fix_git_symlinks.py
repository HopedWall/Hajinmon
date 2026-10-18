"""Replaces git symlink placeholders with copies of their targets.

Usage:
    py tools/fix_git_symlinks.py <repository>

With core.symlinks=false (the Git for Windows default) symlinks are checked
out as small text files containing the target path. Some ReXGlue submodules
(e.g. libmspack) contain symlinked sources, which then fail to compile. This
walks the repository and its submodules and replaces every placeholder with a
copy of the file or directory it points to. Missing targets (e.g. macOS-only
MoltenVK paths) are reported and skipped.
"""
import os
import shutil
import subprocess
import sys


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    repo = sys.argv[1]
    submodules = subprocess.run(
        ["git", "-C", repo, "submodule", "foreach", "--quiet", "--recursive", "echo $displaypath"],
        capture_output=True, text=True, check=True).stdout.split()
    fixed = 0
    for root in [repo] + [os.path.join(repo, s) for s in submodules]:
        files = subprocess.run(["git", "-C", root, "ls-files", "-s"],
                               capture_output=True, text=True, check=True).stdout
        for line in files.splitlines():
            meta, path = line.split("\t", 1)
            if not meta.startswith("120000"):
                continue
            link = os.path.join(root, path)
            if not os.path.isfile(link) or os.path.getsize(link) > 1024:
                continue  # already a real file or directory
            target = open(link, encoding="utf-8").read().strip()
            source = os.path.normpath(os.path.join(os.path.dirname(link), target))
            if not os.path.exists(source):
                print(f"skipped (missing target): {link} -> {target}")
                continue
            os.remove(link)
            if os.path.isdir(source):
                shutil.copytree(source, link)
            else:
                shutil.copyfile(source, link)
            fixed += 1
    print(f"replaced {fixed} symlink placeholders")


if __name__ == "__main__":
    main()
