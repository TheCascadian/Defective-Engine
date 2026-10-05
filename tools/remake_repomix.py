#!/usr/bin/env python3
"""Regenerate minimal repomix output for the repository.

Usage:
  python tools/remake_repomix.py [--output FILE] [--style {xml,markdown}]

Options:
  --output FILE    Output file (default: repomix-output.xml)
  --style STYLE    Output format: xml or markdown (default: xml)
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path


REPOMIX_IGNORES = """
# Build artifacts
build/
build-asan/
build-sanitize/
*.o
*.a
*.so
*.dylib

# Generated files
CMakeFiles/
CMakeCache.txt
cmake_install.cmake
perf_results/
saves/

# Dependencies and caches
node_modules/
.git/

# Tool metadata
.agents/
.claude/
.codegraph/
.github/
.superpowers/
.roo/

# Generated data
repomix-output.xml
Defective-Engine-source.zip
hydrology_*.json

# Test/example directories
examples/
selftest_*/
third_party/

# Config files
.mcp.json
codegraph.json
skills-lock.json
repomix-output.xml
"""


def main():
    parser = argparse.ArgumentParser(
        description="Regenerate minimal repomix output"
    )
    parser.add_argument(
        "--output",
        default="repomix-output.xml",
        help="Output file (default: repomix-output.xml)",
    )
    parser.add_argument(
        "--style",
        choices=["xml", "markdown"],
        default="xml",
        help="Output format (default: xml)",
    )
    args = parser.parse_args()

    repo_root = Path(__file__).parent.parent.resolve()
    os.chdir(repo_root)

    # Check npm is available (for npx)
    try:
        subprocess.run(
            ["npm", "--version"],
            capture_output=True,
            check=True,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        print(
            "Error: npm not found. Install Node.js and run: npm install",
            file=sys.stderr,
        )
        sys.exit(1)

    # Write ignore file
    ignore_file = repo_root / ".repomixignore"
    ignore_file.write_text(REPOMIX_IGNORES.lstrip())
    print(f"Created {ignore_file}")

    # Run repomix
    output_file = repo_root / args.output
    print(f"Regenerating {output_file}...")

    subprocess.run(
        ["npx", "repomix", "--output", str(output_file), "--style", args.style],
        check=True,
    )

    # Show file size
    size = output_file.stat().st_size
    size_str = f"{size / 1024:.1f}KB" if size > 1024 else f"{size}B"
    print(f"Done: {output_file} ({size_str})")


if __name__ == "__main__":
    main()
