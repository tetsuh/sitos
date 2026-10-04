#!/usr/bin/env bash
# Copyright 2026 sitos contributors
# SPDX-License-Identifier: Apache-2.0
#
# Internal-keyword guard (Issue #180). Scans every tracked file of the current Git repository,
# contents and paths, for the prohibited words in KEYWORD_GUARD_WORDS: a comma-separated list of
# single-word terms, matched case-insensitively on word boundaries. All whitespace is removed from
# each term; the masking below relies on accepted terms containing no whitespace. In CI the list comes from the repository secret
# of the same name and is never stored in this repository.
#
# Reporting never discloses the prohibited text: after a valid list is parsed, every reporter
# message is masked, contents hits report the file and an occurrence count, and paths stay
# locatable. Exits 0 for a clean tree and 1 for a hit, a scan failure, or a missing or empty list.
set -e

# GitHub's default bash -e shell does not enable pipefail. Set it
# explicitly so mask_text's grep/sed pipeline returns grep's no-match
# status instead of sed's success, allowing its loop to terminate.
set -o pipefail

WORDS="${KEYWORD_GUARD_WORDS-}"
if [[ -z "$WORDS" ]]; then
  echo "::error::KEYWORD_GUARD_WORDS repository secret is not set; configure it before merging."
  exit 1
fi

words=()
# Split the whole value on commas. A here-string read would stop at the first line break and
# silently drop every later term of a secret stored with line breaks (ADV-206-001), so read up to
# a NUL that never occurs; read then reports end of input, which is expected.
IFS=',' read -r -d '' -a raw < <(printf '%s' "$WORDS") || true
for w in "${raw[@]}"; do
  # Perl's Unicode White_Space property covers non-ASCII separators
  # that the locale-dependent tr [:space:] class can leave behind.
  w="$(printf '%s' "$w" | perl -CSD -pe 's/\p{White_Space}//g')"
  [[ -n "$w" ]] && words+=("$w")
done
if [[ "${#words[@]}" -eq 0 ]]; then
  echo "::error::KEYWORD_GUARD_WORDS word list is empty after parsing; set the repository secret to a comma-separated list before merging."
  exit 1
fi

# GNU grep is the source of truth for case equivalence. Use the
# same locale and literal fixed-string matching for detection and
# masking, including Unicode equivalents such as final sigma and
# long s. Never use Bash lowercasing here: it does not implement
# grep's complete case mapping.
export LC_ALL=C.UTF-8
mask_text() {
  local text="$1" w match prefix output rest replacement=' '
  for w in "${words[@]}"; do
    output=""
    rest="$text"
    # The parser above removes every [:space:] byte before accepting
    # a term. A whitespace replacement is therefore not itself an
    # accepted term and cannot join terms across its boundaries.
    # grep -o may emit every match on one input line; sed selects
    # exactly one match so each iteration consumes actual input.
    while match=$(printf '%s' "$rest" | grep -oaiF -- "$w" | sed -n '1p'); do
      prefix="${rest%%"$match"*}"
      output+="${prefix}${replacement}"
      rest="${rest#*"$match"}"
    done
    text="$output$rest"
  done
  printf '%s' "$text"
}

mask_path() {
  local path="$1"
  mask_text "$path"
}

sanitize_diagnostic() {
  local text="$1"
  # GitHub decodes percent-encoded newlines and carriage returns in
  # workflow commands. Replace those bytes, plus command delimiters,
  # with spaces after masking so attacker-controlled paths remain a
  # single inert line without introducing a new configured term.
  text="${text//$'\r'/ }"
  text="${text//$'\n'/ }"
  text="${text//%/ }"
  text="${text//::/  }"
  printf '%s' "$text"
}

annotation_path_is_safe() {
  local path="$1"
  # A path is a workflow-command property only when it cannot add a
  # property, terminate the property list, or encode a new line.
  case "$path" in
    *[,=%$'\r'$'\n']*|*::*|*=*) return 1 ;;
    *) return 0 ;;
  esac
}

diagnostic_is_safe() {
  local text="$1"
  case "$text" in
    *[,=%$'\r'$'\n']*|*::*) return 1 ;;
    *) return 0 ;;
  esac
}

skeleton_is_safe() {
  local skeleton="$1" w
  for w in "${words[@]}"; do
    # Match each configured term against the actual unmasked
    # skeleton, literally and case-insensitively. Whole-word
    # matching would miss substrings such as "err" and ":".
    if printf '%s' "$skeleton" | grep -qaiF -- "$w"; then
      return 1
    fi
  done
  return 0
}

contains_configured_term() {
  local text="$1" w
  for w in "${words[@]}"; do
    if printf '%s' "$text" | grep -qaiF -- "$w"; then
      return 0
    fi
  done
  return 1
}

report() {
  local annotation_path="$1" message="$2" human masked_raw masked masked_path masked_message skeleton candidate human_is_safe
  if [[ -n "$annotation_path" ]]; then
    human="$annotation_path: $message"
  else
    human="$message"
  fi
  # Mask every human-readable component before applying the
  # rendering safety transformation, preserving the #69 discipline.
  masked_raw=$(mask_text "$human")
  masked=$(sanitize_diagnostic "$masked_raw")
  skeleton='::error file=::'

  # Check both the original and masked diagnostics. Masking can
  # remove a control byte when a configured term spans it, so the
  # original text must also force the inert plain rendering.
  if diagnostic_is_safe "$human" &&
    diagnostic_is_safe "$annotation_path" &&
    diagnostic_is_safe "$masked_raw"; then
    human_is_safe=1
  else
    human_is_safe=0
  fi
  if [[ -n "$annotation_path" ]]; then
    masked_path=$(mask_text "$annotation_path")
    masked_message=$(mask_text "$message")
    if ! annotation_path_is_safe "$masked_path"; then
      printf '%s\n' "$masked"
      return
    fi
    masked_path=$(sanitize_diagnostic "$masked_path")
    masked_message=$(sanitize_diagnostic "$masked_message")
  fi

  # Only the workflow-command skeleton bypasses masking. Assemble
  # the exact rendered command before checking it, so a configured
  # term cannot cross from the skeleton into a masked component (or
  # between any other rendered boundaries).
  if [[ "$human_is_safe" -eq 0 ]] || ! skeleton_is_safe "$skeleton"; then
    printf '%s\n' "$masked"
  elif [[ -n "$annotation_path" ]]; then
    candidate=$(printf '::error file=%s::%s' "$masked_path" "$masked_message")
    if contains_configured_term "$candidate"; then
      printf '%s\n' "$masked"
    else
      printf '%s\n' "$candidate"
    fi
  else
    candidate=$(printf '::error::%s' "$masked")
    if contains_configured_term "$candidate"; then
      printf '%s\n' "$masked"
    else
      printf '%s\n' "$candidate"
    fi
  fi
}

report_plain() {
  local masked
  masked=$(mask_text "$1")
  masked=$(sanitize_diagnostic "$masked")
  printf '%s\n' "$masked"
}

fail=0
scan_dir=$(mktemp -d)
trap 'rm -rf "$scan_dir"' EXIT
tracked_file="$scan_dir/tracked"
if ! git ls-files -z >"$tracked_file" 2>/dev/null; then
  report "" "keyword guard: unable to enumerate tracked files"
  exit 1
fi

while IFS= read -r -d '' f; do
  safe=$(mask_path "$f")
  # A tracked symlink's content is its target text. Scan that text and never follow the link,
  # so the destination's untracked contents neither hide nor cause a hit (ADV-206-002).
  scan_target="$f"
  if [[ -L "$f" ]]; then
    scan_target="$scan_dir/link"
    if ! readlink -- "$f" >"$scan_target" 2>/dev/null; then
      report "" "keyword guard: unable to scan tracked contents"
      fail=1
      continue
    fi
  fi
  for w in "${words[@]}"; do
    # -a: scan binary contents as text; -w: word boundaries;
    # -F: literal; -i: case-insensitive. grep status 1 means
    # ordinary no-match; every other failure is a scan failure.
    matches_file="$scan_dir/matches"
    hits=0
    if grep -oaiwF -- "$w" "$scan_target" >"$matches_file" 2>/dev/null; then
      hits=$(wc -l <"$matches_file")
    else
      grep_status=$?
      if [[ "$grep_status" -ne 1 ]]; then
        report "" "keyword guard: unable to scan tracked contents"
        fail=1
      fi
    fi
    if [[ "$hits" -gt 0 ]]; then
      if [[ "$safe" = "$f" ]]; then
        report "$f" "prohibited word in contents ($hits occurrence(s))"
      else
        report "" "prohibited word in contents of $f ($hits occurrence(s))"
      fi
      fail=1
    fi

    path_file="$scan_dir/path"
    printf '%s' "$f" >"$path_file"
    if grep -oiwF -- "$w" "$path_file" >"$matches_file" 2>/dev/null; then
      phits=$(wc -l <"$matches_file")
    else
      grep_status=$?
      if [[ "$grep_status" -eq 1 ]]; then
        phits=0
      else
        report "" "keyword guard: unable to scan tracked path"
        fail=1
        phits=0
      fi
    fi
    if [[ "$phits" -gt 0 ]]; then
      report "" "prohibited word in path $f ($phits occurrence(s))"
      fail=1
    fi
  done
done <"$tracked_file"

if [[ "$fail" -eq 0 ]]; then
  report_plain "keyword guard: clean"
fi
exit $fail
