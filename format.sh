#!/usr/bin/env bash
set -e
clang-format -style=file -i $(find src test -regex '.*\.\(c\|cpp\|h\)') > /dev/null 2>&1
