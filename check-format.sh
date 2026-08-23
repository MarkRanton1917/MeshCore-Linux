#!/usr/bin/env bash
set -e
clang-format -style=file --dry-run -Werror $(find src test -regex '.*\.\(c\|cpp\|h\)')
