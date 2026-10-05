#!/bin/bash
# Regenerate minimal repomix output
# Usage: ./tools/remake-repomix.sh [output-file]

set -e

OUTPUT_FILE="${1:-repomix-output.xml}"
REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"

cd "$REPO_ROOT"

# Create minimal ignore file
cat > .repomixignore << 'EOF'
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
EOF

echo "Regenerating $OUTPUT_FILE..."
npx repomix --output "$OUTPUT_FILE" --style xml

echo "Done: $OUTPUT_FILE ($(du -h "$OUTPUT_FILE" | cut -f1))"
