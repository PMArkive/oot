#!/usr/bin/env bash
set -euo pipefail

echo "Build failed, attempting to fix BSS ordering..."

.venv/bin/python3 tools/fix_bss.py -v ${VERSION}

echo $pwd
git status
git diff

PATCH=$(git diff | base64 -w 0)

echo $PATCH

if [ -n "$PATCH" ]; then
    echo "\`\`\`" >> $GITHUB_STEP_SUMMARY
    echo "Fixes were made for your PR. To apply these changes to your working directory," >> $GITHUB_STEP_SUMMARY
    echo "copy and run the following command:" >> $GITHUB_STEP_SUMMARY
    echo >> $GITHUB_STEP_SUMMARY
    echo "echo -n $PATCH | base64 -d | git apply -" >> $GITHUB_STEP_SUMMARY
    echo >> $GITHUB_STEP_SUMMARY
    echo "\`\`\`" >> $GITHUB_STEP_SUMMARY
fi

exit 1
