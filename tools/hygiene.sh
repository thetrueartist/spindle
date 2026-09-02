#!/usr/bin/env bash
# Repository hygiene: refuses what must never be published.
#
#   tools/hygiene.sh            scan every tracked file
#   tools/hygiene.sh --staged   scan what is about to be committed
#   tools/hygiene.sh --message FILE   scan a commit message
#
# The rules here are generic on purpose. Anything personal (a name, a
# machine, a place) belongs in SPINDLE_HYGIENE_DENY, a regular expression
# read from the environment and never stored in the repository, because a
# denylist that named the things to hide would itself be the leak.
set -u
export LC_ALL=C
fail=0

# check_text: reads content on stdin, prints any problems, returns 1 if it
# found any. It must RETURN rather than set a global, because the staged
# path feeds it through a pipe, which runs it in a subshell where a global
# would be lost. Callers do `... | check_text "$label" || fail=1`.
check_text() {
  local label="$1" c bad=0
  c=$(cat)
  report() { printf 'hygiene: %s: %s\n' "$label" "$1"; bad=1; }

  printf '%s' "$c" | grep -qiE 'claude|anthropic|openai|chatgpt|copilot|co-authored-by|generated with \[|as an ai' \
    && report "assistant reference"
  printf '%s' "$c" | grep -qP '\xE2\x80[\x93\x94]' \
    && report "em or en dash"
  printf '%s' "$c" | grep -qP '[A-Za-z]:\\\\Users\\\\(?!(Public|Default|All Users|some|someone|sam|test|user|name|you|me|example|john|jane)\b|<)[A-Za-z0-9._-]+' \
    && report "user profile path"
  printf '%s' "$c" | grep -qE '/home/[a-z][a-z0-9_-]*/|/root/\.|/tmp/[a-z]+-[0-9]' \
    && report "development machine path"
  printf '%s' "$c" | grep -oE '[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}' | grep -qv 'users\.noreply\.github\.com' \
    && report "email address"
  printf '%s' "$c" | grep -qE 'PRIVATE KEY|ghp_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}|AKIA[0-9A-Z]{16}|xox[baprs]-[A-Za-z0-9-]{10,}' \
    && report "secret material"
  if [ -n "${SPINDLE_HYGIENE_DENY:-}" ]; then
    printf '%s' "$c" | grep -qiE -- "$SPINDLE_HYGIENE_DENY" \
      && report "matches the private denylist"
  fi
  return "$bad"
}

skip_file() {
  case "$1" in
    # Binary media carry no prose.
    docs/*.png|docs/*.gif|docs/*.mp4|*.ico) return 0 ;;
    # The checker itself defines the very patterns it hunts for, so it
    # would always match itself. Its own rules are reviewed by reading it.
    tools/hygiene.sh) return 0 ;;
  esac
  return 1
}

case "${1:-}" in
  --message)
    check_text "commit message" < "$2" || fail=1
    ;;
  --staged)
    while IFS= read -r f; do
      [ -z "$f" ] && continue
      skip_file "$f" && continue
      git show ":$f" 2>/dev/null | check_text "$f" || fail=1
    done < <(git diff --cached --name-only --diff-filter=ACMR)
    ;;
  *)
    while IFS= read -r f; do
      [ -z "$f" ] && continue
      skip_file "$f" && continue
      check_text "$f" < "$f" || fail=1
    done < <(git ls-files)
    ;;
esac

if [ "$fail" -ne 0 ]; then
  echo "hygiene: refused. Fix the lines above, or, for a deliberate exception, explain it in the commit and use --no-verify."
  exit 1
fi
echo "hygiene: clean"
