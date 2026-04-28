#!/usr/bin/env bash

# Test runner for Symbol (.sym) files
# examples/ — expected to exit 0 (success)
# errors/   — expected to exit non-zero (failure)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SI="$SCRIPT_DIR/symboli"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[0;33m'
BOLD='\033[1m'
RESET='\033[0m'

passed=0
failed=0

run_test() {
    local file="$1"
    local expect_fail="$2"   # 1 = errors/, 0 = examples/
    local name="${file#$SCRIPT_DIR/}"

    local output
    output=$(echo "" | "$SI" "$file" 2>&1)
    local exit_code=$?

    local ok=0
    if [ "$expect_fail" -eq 1 ] && [ "$exit_code" -ne 0 ]; then
        ok=1
    elif [ "$expect_fail" -eq 0 ] && [ "$exit_code" -eq 0 ]; then
        ok=1
    fi

    if [ "$ok" -eq 1 ]; then
        echo -e "  ${GREEN}PASS${RESET}  $name"
        ((passed++))
    else
        echo -e "  ${RED}FAIL${RESET}  $name"
        if [ "$expect_fail" -eq 1 ]; then
            echo -e "         ${YELLOW}(expected non-zero exit, got $exit_code)${RESET}"
        else
            echo -e "         ${YELLOW}(expected exit 0, got $exit_code)${RESET}"
        fi
        if [ -n "$output" ]; then
            echo "$output" | sed 's/^/         /'
        fi
        ((failed++))
    fi
}

echo -e "\n${BOLD}examples/${RESET} — должны выполняться без ошибок"
echo "────────────────────────────────────────────"
for f in "$SCRIPT_DIR/examples/"*.sym; do
    [ -f "$f" ] || continue
    run_test "$f" 0
done

echo
echo -e "${BOLD}errors/${RESET} — должны завершаться с ошибкой"
echo "────────────────────────────────────────────"
for f in "$SCRIPT_DIR/errors/"*.sym; do
    [ -f "$f" ] || continue
    run_test "$f" 1
done

echo
echo "════════════════════════════════════════════"
total=$((passed + failed))
echo -e "  ${BOLD}Итого: $total  ${GREEN}$passed passed${RESET}  ${RED}$failed failed${RESET}"
echo

[ "$failed" -eq 0 ]
