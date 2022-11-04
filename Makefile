all: exFAT_OS_Read_Operate

fileSystemReader: exFAT_OS_Read_Operate.c
	clang exFAT_OS_Read_Operate.c -Wall -Wpedantic -Wextra -Werror -o exFAT_OS_Read_Operate

clean:
	rm -f exFAT_OS_Read_Operate
