#!/bin/sh
# One GitHub issue per kind of finding, for .github/workflows/security.yml (needs gh and GH_TOKEN):
#   security/issue.sh open  TITLE BODY_FILE   opens it, or updates it and comments when the findings changed
#   security/issue.sh close TITLE             closes it when the findings are gone
# The body ends with a digest of the findings, so an unchanged finding does not notify every week.
set -eu
action=$1
title=$2
number=$(gh issue list --state open --limit 200 --json number,title \
         --jq ".[] | select(.title == \"$title\") | .number" | head -n 1)

case $action in
open)
    digest=$(sha256sum "$3" | cut -c1-16)
    body=$(mktemp)
    { cat "$3"; echo; echo "<!-- findings: $digest -->"; } > "$body"
    if [ -z "$number" ]; then
        gh issue create --title "$title" --body-file "$body"
    elif ! gh issue view "$number" --json body --jq .body | grep -q "findings: $digest"; then
        gh issue edit "$number" --body-file "$body"
        gh issue comment "$number" --body "The findings changed on $(date -u +%Y-%m-%d); the description lists them."
    fi
    ;;
close)
    if [ -n "$number" ]; then
        gh issue close "$number" --comment "The check of $(date -u +%Y-%m-%d) finds nothing more."
    fi
    ;;
*)
    echo "usage: $0 open TITLE BODY_FILE | close TITLE" >&2
    exit 2
    ;;
esac
