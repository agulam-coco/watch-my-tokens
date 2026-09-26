#!/usr/bin/env bash
#
# AgentPager PreToolUse hook.
# Reads the tool-call JSON Claude Code sends on stdin, asks the
# AgentPager bridge whether to allow it, and prints the decision
# back to Claude Code in the format it expects.

set -euo pipefail

BRIDGE_URL="http://localhost:4545/approval"
CURL_TIMEOUT=28   # a hair above the bridge's own 25s approval timeout

INPUT_JSON="$(cat)"

TOOL_NAME=$(echo "$INPUT_JSON" | jq -r '.tool_name // "unknown"')
COMMAND=$(echo "$INPUT_JSON" | jq -r '.tool_input.command // .tool_input.description // "(no command)"')

# Build the request body for the bridge.
REQUEST_BODY=$(jq -n --arg command "$COMMAND" --arg tool "$TOOL_NAME" \
  '{command: ($tool + ": " + $command)}')

# Call the bridge. If it's unreachable or times out, fail open to
# "escalate" so Claude Code falls back to its normal permission prompt
# instead of silently blocking or silently allowing.
RESPONSE=$(curl -sS --max-time "$CURL_TIMEOUT" \
  -X POST "$BRIDGE_URL" \
  -H "Content-Type: application/json" \
  -d "$REQUEST_BODY" 2>/dev/null || echo '{"decision":"ask"}')

DECISION=$(echo "$RESPONSE" | jq -r '.decision // "ask"')

case "$DECISION" in
  allow)
    PERMISSION="allow"
    REASON="Approved on AgentPager device"
    ;;
  deny)
    PERMISSION="deny"
    REASON="Denied on AgentPager device"
    ;;
  *)
    PERMISSION="escalate"
    REASON="AgentPager did not respond in time; falling back to normal prompt"
    ;;
esac

jq -n --arg perm "$PERMISSION" --arg reason "$REASON" '{
  hookSpecificOutput: {
    hookEventName: "PreToolUse",
    permissionDecision: $perm,
    permissionDecisionReason: $reason
  }
}'

exit 0