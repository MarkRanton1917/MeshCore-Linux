# Sources only. The globs used to be src/* and src/room/* and so on, which hands
# clang-format every README in the tree as well — and it happily reformats them
# into rubble. Same file list as check-format.sh, so what is formatted here is
# exactly what is checked there.
clang-format -style=file -i $(find src test -regex '.*\.\(c\|cpp\|h\)') > /dev/null 2>&1
