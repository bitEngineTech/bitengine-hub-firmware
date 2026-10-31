#!/bin/bash
# components/matter_ctrl/test_host/run.sh —— matter_ctrl 纯逻辑的主机测试（不依赖 IDF / CHIP）
set -e
cd "$(dirname "$0")"
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I../include -o "$OUT/mc_ident_test" mc_ident_test.c ../mc_ident_core.c
"$OUT/mc_ident_test"
gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -o "$OUT/mc_nodes_test" mc_nodes_test.c ../mc_nodes_core.c ../mc_code_core.c
"$OUT/mc_nodes_test"
gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I../include -o "$OUT/mc_iv_test" mc_iv_test.c ../mc_iv_core.c
"$OUT/mc_iv_test"
gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g -I../include -o "$OUT/mc_sub_test" mc_sub_test.c ../mc_sub_core.c ../mc_iv_core.c -lm
"$OUT/mc_sub_test"
