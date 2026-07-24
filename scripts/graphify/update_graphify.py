#!/usr/bin/env python3

import os
import subprocess
import sys
from pathlib import Path


def run_command(cmd, check=True):
    res = subprocess.run(cmd, shell=True, capture_output=True, text=True, check=False)
    if check and res.returncode != 0:
        print(f"Error executing: {cmd}\n{res.stderr}", file=sys.stderr)
        sys.exit(res.returncode)
    return res.stdout.strip()


def main():
    repo_root = Path(__file__).resolve().parent.parent.parent
    os.chdir(repo_root)

    print("Updating graphify AST extraction...")
    run_command("graphify update .")

    print("Rebuilding graphify Wiki...")
    graphify_out = repo_root / "graphify-out"
    if not graphify_out.exists():
        print(f"Error: {graphify_out} directory not found.", file=sys.stderr)
        sys.exit(84)

    wiki_script = repo_root / "scripts" / "graphify" / "update_wiki.py"
    res = run_command(f"{sys.executable} {wiki_script}")
    print(res)


if __name__ == "__main__":
    main()
