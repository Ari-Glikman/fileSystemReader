# fileSystemReader
read and operate on exFAT file system


This program obtains information from an exFAT volume. The exFAT volume must be stored in the same directory as the executable program that will be created. To compile the executable using the Makefile enter 'make' into the command line. There are 3 options that a user can run the executable with:

1. './exFAT_OS_Read_Operate <exFATVolume> <info>' will print out basic information of the exFAT volme.
2. './exFAT_OS_Read_Operate <exFATVolume> <list>' will print out (in an ordered fashion) the directories and files contained at each level (that is, the root, and then within each directory)
3. './exFAT_OS_Read_Operate <exFATVolume> <get> <path/to/"file name.txt"> will duplicate the requested file from the volume onto the current working directory that the executable is in. Note that if a file or directory name has spaces it must use quotations to hold the argument together. As well one may choose to use </path/to/"file name.txt"> noting that the first slash is optional. The new file will have the same file name that it contains in the exFAT volume so ensure that no file name with the same name is present in the directory before running this instruction.


