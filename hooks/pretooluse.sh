#!/usr/bin/env bash
#
# pretooluse.sh - Claude Code PreToolUse hook for watch-my-tokens (AgentPager).
#
# Asks the bridge server whether a tool call may run, so that the decision can
# be made on the device. Register it in ~/.claude/settings.json under
# hooks.PreToolUse with matcher "Bash" and a timeout of at least 30 s.
#
# Input (stdin):
#   The PreToolUse JSON that Claude Code sends, e.g.
#   {"tool_name": "Bash", "tool_input": {"command": "npm test", ...}, ...}
#
# Output (stdout):
#   {"hookSpecificOutput": {"hookEventName": "PreToolUse",
#                           "permissionDecision": "allow" | "deny" | "ask",
#                           "permissionDecisionReason": "<text>"}}
#
# Exit status:
#   Always 0. The decision is carried in the JSON. If the bridge is down,
#   times out or returns anything unexpected, the decision is "ask", so Claude
#   Code shows its normal permission prompt. The hook never auto-allows.
#
# Dependencies: bash, curl, jq.

set -euo pipefail

BRIDGE_URL="http://localhost:4545/approval"
CURL_TIMEOUT=28   # a little longer than the bridge's own 25 s approval timeout

INPUT_JSON="$(cat)"

# Pull out the tool name and the command to show on the device. Tools without
# a "command" field fall back to their "description".
TOOL_NAME=$(echo "$INPUT_JSON" | jq -r '.tool_name // "unknown"')
COMMAND=$(echo "$INPUT_JSON" | jq -r '.tool_input.command // .tool_input.description // "(no command)"')

# Request body for the bridge, e.g. {"command": "Bash: npm test"}.
REQUEST_BODY=$(jq -n --arg command "$COMMAND" --arg tool "$TOOL_NAME" \
  '{command: ($tool + ": " + $command)}')

# Ask the bridge and block until it answers. If it is unreachable or times
# out, use {"decision":"ask"} so Claude Code falls back to its normal prompt.
RESPONSE=$(curl -sS --max-time "$CURL_TIMEOUT" \
  -X POST "$BRIDGE_URL" \
  -H "Content-Type: application/json" \
  -d "$REQUEST_BODY" 2>/dev/null || echo '{"decision":"ask"}')

DECISION=$(echo "$RESPONSE" | jq -r '.decision // "ask"')

# Map the bridge's decision to Claude Code's permissionDecision values.
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
    PERMISSION="ask"
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
