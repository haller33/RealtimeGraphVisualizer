#!/usr/bin/env bash
# Stress test for the graph visualizer API.
# Usage: ./stress_test.sh [num_requests] [concurrency]

API_URL="http://localhost:5000"
NUM_REQUESTS=${1:-100}      # default 100 requests
CONCURRENCY=${2:-10}         # default 10 parallel requests

echo "Starting stress test: $NUM_REQUESTS requests, concurrency $CONCURRENCY"
echo "API endpoint: $API_URL"
echo "----------------------------------------"

# Create a temporary file with all request data
TEMP_FILE=$(mktemp)

# Function to generate a random node JSON
generate_node() {
    local id=$1
    local label="Node$id"
    local tag1="tag$((RANDOM % 20))"
    local tag2="tag$((RANDOM % 20))"
    cat <<EOF
{"method":"POST","url":"$API_URL/nodes","data":{"id":"$id","label":"$label","tags":["$tag1","$tag2"],"metadata":{"rand":$RANDOM}}}
EOF
}

# Function to generate a random edge JSON (between two existing nodes)
generate_edge() {
    local src=$((RANDOM % NUM_REQUESTS))
    local tgt=$((RANDOM % NUM_REQUESTS))
    [[ $src -eq $tgt ]] && tgt=$(( (src+1) % NUM_REQUESTS ))
    cat <<EOF
{"method":"POST","url":"$API_URL/edges","data":{"source":"$src","target":"$tgt"}}
EOF
}

# Build the worklist: mix of nodes (80%) and edges (20%)
for i in $(seq 1 $NUM_REQUESTS); do
    if (( RANDOM % 100 < 80 )); then
        generate_node "$i" >> "$TEMP_FILE"
    else
        generate_edge >> "$TEMP_FILE"
    fi
done

# Run the requests in parallel using xargs
cat "$TEMP_FILE" | parallel -j "$CONCURRENCY" --halt-on-error 0 '
    data=$(echo {} | jq -c .data)
    curl -s -X $(echo {} | jq -r .method) \
         -H "Content-Type: application/json" \
         -d "$data" \
         $(echo {} | jq -r .url) > /dev/null
    echo -n "."
'

echo -e "\n----------------------------------------"
echo "Stress test completed."
rm -f "$TEMP_FILE"
