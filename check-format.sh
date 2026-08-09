clang-format -style=file --dry-run -Werror `find src -regex '.*\.\(c\|cpp\|h\)'`
clang-format -style=file --dry-run -Werror `find test -regex '.*\.\(c\|cpp\|h\)'`
