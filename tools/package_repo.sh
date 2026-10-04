#!/usr/bin/env bash
set -euo pipefail

# Create small source snapshots without build products, saves, or generated files.
# Usage: tools/package_repo.sh [--zip FILE] [--repomix FILE]

root=$(git rev-parse --show-toplevel 2>/dev/null || pwd)
root=${root%/}
name=$(basename "$root")
zip_out="$root/${name}-source.zip"
xml_out="$root/repomix-output.xml"

while (($#)); do
  case "$1" in
    --zip) zip_out=$2; shift 2 ;;
    --repomix) xml_out=$2; shift 2 ;;
    -h|--help) printf 'usage: %s [--zip FILE] [--repomix FILE]\n' "$0"; exit 0 ;;
    *) printf 'unknown option: %s\n' "$1" >&2; exit 2 ;;
  esac
done

mkdir -p "$(dirname "$zip_out")" "$(dirname "$xml_out")"
zip_out=$(cd "$(dirname "$zip_out")" && pwd)/$(basename "$zip_out")
xml_out=$(cd "$(dirname "$xml_out")" && pwd)/$(basename "$xml_out")
rm -f "$zip_out" "$xml_out"

(cd "$(dirname "$root")" && zip -qr "$zip_out" "$name" \
  -x "$name/.git/*" "$name/build/*" "$name/build-*/*" "$name/saves/*" \
     "$name/perf_results/*" "$name/selftest_*/*" "$name/settings.json" "$name/*.o" \
     "$name/$name-source.zip" "$name/repomix-output.xml")

xml_escape() {
  sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' -e 's/>/\&gt;/g'
}

{
  printf '<repomix>\n'
  while IFS= read -r file; do
    rel=${file#"$root/"}
    case "$rel" in
      .git/*|build/*|build-*/*|saves/*|perf_results/*|selftest_*/*|third_party/*|engine_assets/*|docs/superpowers/*|*/assets/*|settings.json|repomix-output.xml|*.o|*.a|*.so|*.png|*.jpg|*.jpeg|*.gif|*.webp|*.bmp|*.ttf|*.zip) continue ;;
    esac
    [ -f "$root/$rel" ] || continue
    if LC_ALL=C grep -Iq . "$root/$rel"; then
      printf '<file path="%s">' "$(printf '%s' "$rel" | xml_escape)"
      awk 'NF' "$root/$rel" | xml_escape
      printf '</file>\n'
    fi
  done < <(find "$root" -type f \
    -not -path "$root/.git/*" \
    -not -path "$zip_out" \
    -not -path "$xml_out" \
    -not -path "$root/third_party/*" \
    -not -path "$root/engine_assets/*" \
    -not -path "$root/docs/superpowers/*" \
    -not -path '*/assets/*' \
    -not -name 'repomix-output.xml' \
    -print | sort)
  printf '</repomix>\n'
} > "$xml_out"

printf 'created %s\ncreated %s\n' "$zip_out" "$xml_out"
