all: fileSystemReader

fileSystemReader: fileSystemReader.c
	clang fileSystemReader.c -Wall -Wpedantic -Wextra -Werror -o fileSystemReader

clean:
	rm -f fileSystemReader
