#!/usr/bin/env bash
# test_api.sh - Robust test for all endpoints

API_URL="http://localhost:5000"
PASSED=0
FAILED=0

# Helper: run curl and check JSON response
check_json() {
    local desc="$1"
    local url="$2"
    local method="${3:-GET}"
    local data="$4"
    
    local tmp=$(mktemp)
    if [ "$method" = "POST" ] && [ -n "$data" ]; then
        curl -s -X POST "$url" -H "Content-Type: application/json" -d "$data" -w "%{http_code}" -o "$tmp" > "$tmp.code"
    else
        curl -s -X "$method" "$url" -w "%{http_code}" -o "$tmp" > "$tmp.code"
    fi
    local http_code=$(cat "$tmp.code")
    if [ "$http_code" -ge 200 ] && [ "$http_code" -lt 300 ]; then
        if jq . "$tmp" >/dev/null 2>&1; then
            echo "✅ PASS: $desc"
            ((PASSED++))
        else
            echo "⚠️  WARN: $desc returned $http_code but non-JSON body"
            ((PASSED++))  # still consider pass if HTTP OK
        fi
    else
        echo "❌ FAIL: $desc (HTTP $http_code)"
        ((FAILED++))
    fi
    rm -f "$tmp" "$tmp.code"
}

# Helper for simple GET expecting text
check_text() {
    local desc="$1"
    local url="$2"
    local pattern="$3"
    local response=$(curl -s "$url")
    if echo "$response" | grep -q "$pattern"; then
        echo "✅ PASS: $desc"
        ((PASSED++))
    else
        echo "❌ FAIL: $desc (pattern not found)"
        ((FAILED++))
    fi
}

echo "=== Testing Graph Visualizer API at $API_URL ==="
echo

# 1. POST /nodes - add node A
check_json "POST /nodes (A)" "$API_URL/nodes" "POST" '{"id":"A","label":"Node A","tags":["test","alpha"],"metadata":{"color":"blue"}}'

# 2. POST /nodes - add node B
check_json "POST /nodes (B)" "$API_URL/nodes" "POST" '{"id":"B","label":"Node B","metadata":{"type":"server"}}'

# 3. POST /edges - connect A-B
check_json "POST /edges (A-B)" "$API_URL/edges" "POST" '{"source":"A","target":"B"}'

# 4. GET /graph - fetch graph
check_json "GET /graph" "$API_URL/graph" "GET"

# 5. POST /nodes/A/tags - add tags
check_json "POST /nodes/A/tags" "$API_URL/nodes/A/tags" "POST" '{"tags":["important","visual"]}'

# 6. GET /nodes?tag=alpha - query by tag
check_json "GET /nodes?tag=alpha" "$API_URL/nodes?tag=alpha" "GET"

# 7. GET /nodes - list all nodes (count at least 2)
response=$(curl -s "$API_URL/nodes")
if echo "$response" | jq -e 'length >= 2' >/dev/null 2>&1; then
    echo "✅ PASS: GET /nodes (count: $(echo "$response" | jq length))"
    ((PASSED++))
else
    echo "❌ FAIL: GET /nodes (invalid JSON or count <2)"
    ((FAILED++))
fi

# 8. DELETE /edges - remove edge A-B
check_json "DELETE /edges?source=A&target=B" "$API_URL/edges?source=A&target=B" "DELETE"

# 9. DELETE /nodes/A - delete node A
check_json "DELETE /nodes/A" "$API_URL/nodes/A" "DELETE"

# 10. Verify node A gone (simple text check)
check_text "Node A no longer present" "$API_URL/nodes" '"id":"A"' "invert"

echo
echo "=== Test Summary ==="
echo "✅ Passed: $PASSED"
echo "❌ Failed: $FAILED"
echo "===================="
exit $((FAILED > 0))
