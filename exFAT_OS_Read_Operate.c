//-----------------------------------------
// BY: Ariel Glikman
//
// This reads an exFAT file system and can do some operations with it.
// The program can:
// 1. give information about the file system
// 2. list all the files and directories contained within it (ordered how they are stored)
// 3. Extract a file from the file system to the directory the program runs in
//
//-----------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <assert.h>

#define FILE_BIT_OFFSET 16          //bit set if file is file, else directory i.e. base 2: 0001 0000
#define ALLOCATION_BITMAP_ENTRY 129 //0x81
#define VOLUME_LABEL_ENTRY 131      //0x83
#define FILE_TYPE_ENTRY 133         //0x85
#define BYTES_PER_ENTRY 32

#define CLUSTER_INDEX_OFFSET 2
#define VOLUME_LABEL_CHARS 11
#define UNICODE_CHARS_PER_ENTRY 15
#define ASCII_TO_UNICODE_CHAR_RATIO 2
#define MAX_ASCII_STRING_SIZE 255

#define MAX_BYTE_VALUE 255
#define BITS_PER_BYTE 8
#define BYTES_PER_KB 1024

#define PERMISSIONS 0644

uint32_t serialNumber;
uint32_t rootDirectory;  //recall that FAT[X] corresponds to Cluster[X-2]
uint32_t clstHeapOffset; //offset to data region in sectors
uint32_t fatOffset;      // in sectors
uint32_t clusterCount;   // number of clusters

int bytesPerSector;
int sectorsPerCluster;
char *volumeLabel;
char *path; //used for get instruction
int freeSpaceKB;

typedef enum bool
{
    false,
    true
} bool;

/**
 * Convert a Unicode-formatted string containing only ASCII characters
 * into a regular ASCII-formatted string (16 bit chars to 8 bit 
 * chars).
 *
 * NOTE: this function does a heap allocation for the string it 
 *       returns, caller is responsible for `free`-ing the allocation
 *       when necessary.
 *
 * uint16_t *unicode_string: the Unicode-formatted string to be 
 *                           converted.
 * uint8_t   length: the length of the Unicode-formatted string (in
 *                   characters).
 *
 * returns: a heap allocated ASCII-formatted string.
 */
static char *unicode2ascii(uint16_t *unicode_string, uint8_t length)
{
    assert(unicode_string != NULL);
    assert(length > 0);
    
    char *ascii_string = NULL;

    if (unicode_string != NULL && length > 0)
    {
        // +1 for a NULL terminator
        ascii_string = calloc(sizeof(char), length + 1);

        if (ascii_string)
        {
            // strip the top 8 bits from every character in the
            // unicode string
            for (uint8_t i = 0; i < length; i++)
            {
                ascii_string[i] = (char)unicode_string[i];
            }
            // stick a null terminator at the end of the string.
            ascii_string[length] = '\0';
        }
    }

    return ascii_string;
}

//input: file descriptor of exFAT volume
void getSerialNumber(int fd)
{
    lseek(fd, 100, SEEK_SET);
    read(fd, &serialNumber, 4);
}

//input: file descriptor of exFAT volume
void getRootDirectory(int fd)
{
    lseek(fd, 96, SEEK_SET);
    read(fd, &rootDirectory, 4);
}

//return offset in bytes from start of volume to cluster
long findOffsetToCluster(int cluster)
{
    long offset = (clstHeapOffset + ((cluster - CLUSTER_INDEX_OFFSET) * sectorsPerCluster)) * bytesPerSector;
    return offset;
}

//------------------------------------------------------
// clusterHeapOffset
//
// PURPOSE: Find the offset in sectors the beginning of the cluster heap / data region as well as the count of clusters in the volume and set the global variable to the appropriate value
// INPUT PARAMETERS:
//     file descriptor of exFAT volume
//------------------------------------------------------
void clusterHeapOffset(int fd)
{
    lseek(fd, 88, SEEK_SET);
    read(fd, &clstHeapOffset, 4);
    read(fd, &clusterCount, 4);
}

//------------------------------------------------------
// sectorsPerClus
//
// PURPOSE: Find how many sectors there are per cluster and set the global variable to the appropriate value
// INPUT PARAMETERS:
//     file descriptor of exFAT volume
// OUTPUT PARAMETERS:
//      number of unset bits (0 - 8)
//------------------------------------------------------
void sectorsPerClus(int fd)
{
    uint8_t powerOfTwoClst;     // 2^powerOfTwoClst = sectors per cluster
    uint8_t powerOfTwoSecBytes; // 2^powerOfTwoSecBytes = bytes per sector
    int sctPerClst = 1;         // minimum
    int bytesPerSec = 1;        // minimum
    int sizeInBytesOfCluster;

    lseek(fd, 108, SEEK_SET);
    read(fd, &powerOfTwoSecBytes, 1);
    // 2 ^ powerOfTwoSecBytes
    bytesPerSec = bytesPerSec << powerOfTwoSecBytes;
    bytesPerSector = bytesPerSec;
    read(fd, &powerOfTwoClst, 1);
    //2 ^ powerOfTwoClst
    sctPerClst = sctPerClst << powerOfTwoClst;
    sectorsPerCluster = sctPerClst;
    sizeInBytesOfCluster = sctPerClst * bytesPerSec;
}

//input: file descriptor of exFAT volume
void getFatOffset(int fd)
{
    lseek(fd, 80, SEEK_SET);
    read(fd, &fatOffset, 4);
}

//input: file descriptor of exFAT volume and the current cluster
//this returns the value stored at FAT[currCluster].
//recall that the correct cluster index is the value returned - 2 for historical reasons.
uint32_t nextCluster(int fd, uint32_t currCluster)
{
    uint32_t next;

    lseek(fd, fatOffset * bytesPerSector, SEEK_SET); //to start of FAT
    lseek(fd, 4 * currCluster, SEEK_CUR);            //to correct FAT index. each cluster in fat is 4 bytes
    read(fd, &next, 4);

    return next;
}

//------------------------------------------------------
// getVolumeLabel
//
// PURPOSE: Set the global volume label string to the appropriate label
// INPUT PARAMETERS:
//     file descriptor of exFAT volume
//------------------------------------------------------
void getVolumeLabel(int fd)
{
    uint16_t unicodeString[VOLUME_LABEL_CHARS];
    uint8_t length;
    uint8_t entryType;
    bool found = false;
    long bytesReadInCluster = 0;
    uint32_t currCluster = rootDirectory;
    lseek(fd, findOffsetToCluster(currCluster), SEEK_SET);
    while (!found)
    {
        read(fd, &entryType, 1);
        bytesReadInCluster += 1;
        if (entryType == VOLUME_LABEL_ENTRY)
        {
            found = true;
        }
        else
        {
            lseek(fd, BYTES_PER_ENTRY - bytesReadInCluster, SEEK_CUR);
            bytesReadInCluster += BYTES_PER_ENTRY - bytesReadInCluster;
        }

        if (bytesReadInCluster == (bytesPerSector * sectorsPerCluster)) //go to next cluster
        {
            currCluster = nextCluster(fd, currCluster);
            lseek(fd, findOffsetToCluster(currCluster), SEEK_SET);
            bytesReadInCluster = 0;
        }
    }
    read(fd, &length, 1);
    read(fd, &unicodeString, length * ASCII_TO_UNICODE_CHAR_RATIO);
    volumeLabel = unicode2ascii(unicodeString, length);
}

//------------------------------------------------------
// countOffBits
//
// PURPOSE: Count how many bits of a byte are unset
// INPUT PARAMETERS:
//     the byte
// OUTPUT PARAMETERS:
//      number of unset bits (0 - 8)
//------------------------------------------------------
long countOffBits(uint8_t byte)
{
    long count = 0;
    uint8_t myByte = byte;
    for (int x = 1; x <= MAX_BYTE_VALUE; x = x << 1)
    {
        if ((x & myByte) == 0)
            count++;
    }
    return count;
}

//------------------------------------------------------
// getEmptys
//
// PURPOSE: Count the unset bits of the bitmap to find unused cluster count
// INPUT PARAMETERS:
//     the offset to the bitmap (start of cluster), file descriptor of exFAT volume, the cluster to start at
//------------------------------------------------------
void getEmptys(long offset, int fd, uint32_t currCluster)
{
    uint8_t currByte;
    long emptys = 0;
    lseek(fd, offset, SEEK_SET);
    int bytesReadInCluster = 0;

    for (int i = 0; i < (int)(clusterCount / BITS_PER_BYTE); i++)
    {
        read(fd, &currByte, 1);
        emptys += countOffBits(currByte);
        if (bytesReadInCluster == (bytesPerSector * sectorsPerCluster)) //go to next cluster
        {
            currCluster = nextCluster(fd, currCluster);
            lseek(fd, findOffsetToCluster(currCluster), SEEK_SET);
            bytesReadInCluster = 0;
        }
    }
    long freeSpace = emptys * sectorsPerCluster * bytesPerSector;
    freeSpaceKB = freeSpace / BYTES_PER_KB;
}
//------------------------------------------------------
// allocationBitMap
//
// PURPOSE: Find where the desired allocation bit map entry is.
// INPUT PARAMETERS:
//     file descriptor of exFAT volume
//------------------------------------------------------
void allocationBitMap(int fd)
{
    int myOffset = (clstHeapOffset + ((rootDirectory - CLUSTER_INDEX_OFFSET) * sectorsPerCluster)) * bytesPerSector;
    uint8_t entryType;
    bool found = false;
    uint32_t firstCluster;
    uint32_t currCluster = rootDirectory;
    int bytesOffset;

    long bytesReadInCluster = 0;
    lseek(fd, myOffset, SEEK_SET);
    while (!found) //search for allocation bit map entry
    {
        read(fd, &entryType, 1);
        bytesReadInCluster += 1;

        if (entryType == ALLOCATION_BITMAP_ENTRY)
        {
            found = true;

            lseek(fd, 19, SEEK_CUR);
            bytesReadInCluster += 19;
            read(fd, &firstCluster, 4);
            bytesOffset = (clstHeapOffset + (firstCluster - CLUSTER_INDEX_OFFSET)) * bytesPerSector;

            getEmptys(bytesOffset, fd, currCluster);
        }
        else
        {
            lseek(fd, BYTES_PER_ENTRY - bytesReadInCluster, SEEK_CUR);
            bytesReadInCluster += BYTES_PER_ENTRY - bytesReadInCluster;
        }
        if (bytesReadInCluster == (bytesPerSector * sectorsPerCluster)) //go to next cluster
        {
            currCluster = nextCluster(fd, currCluster);
            lseek(fd, findOffsetToCluster(currCluster), SEEK_SET);
            bytesReadInCluster = 0;
        }
    }
}

//------------------------------------------------------
// info
//
// PURPOSE: Called to execute the info command
// INPUT PARAMETERS:
//     file descriptor of exFAT volume
//------------------------------------------------------
void info(int fd)
{
    getSerialNumber(fd);
    getRootDirectory(fd);
    sectorsPerClus(fd);
    clusterHeapOffset(fd);
    getFatOffset(fd);
    getVolumeLabel(fd);
    allocationBitMap(fd);
}

//------------------------------------------------------
// listRecurse
//
// PURPOSE: Traverse the file system in a depth first manner. When a directory is found print the name and then find and print the file/directories it contains recursively
// INPUT PARAMETERS:
//     file descriptor of exFAT volume, the cluster to look at (start with rootDirectory in general), how many levels have been searched (0 to start)
//------------------------------------------------------
void listRecurse(int fdOrig, int firstCluster, int levels)
{
    int fd = fdOrig; //we will keep a second file pointer so that when we return from the recusive call the pointer is not affected
    int currCluster = firstCluster;

    //include bytes we 'skip' over in order to track whether to go to next cluster/entry or not
    int bytesReadInCluster = 0;
    int bytesReadInEntry = 0;
    char* asciiString;

    bool done = false;

    uint8_t currEntryType;
    uint8_t secondaryCount;  //to know how many file name directories there is
    uint32_t nextDirCluster; // value from FAT
    uint8_t nameLength;
    bool directory;
    uint16_t unicodeString[MAX_ASCII_STRING_SIZE];

    
    sectorsPerClus(fd);
    clusterHeapOffset(fd);
    getFatOffset(fd);

    lseek(fd, findOffsetToCluster(firstCluster), SEEK_SET);

    while (!done)
    {
        read(fd, &currEntryType, 1);
        bytesReadInCluster += 1;
        bytesReadInEntry += 1;
        if (currEntryType == FILE_TYPE_ENTRY)
        {
            read(fd, &secondaryCount, 1);
            bytesReadInEntry += 1;
            bytesReadInCluster += 1;

            lseek(fd, 2, SEEK_CUR); //skip 2 bytes of set checksum
            bytesReadInEntry += 2;
            bytesReadInCluster += 2;
            uint16_t fileAttributes;
            read(fd, &fileAttributes, 2);
            bytesReadInEntry += 2;
            bytesReadInCluster += 2;

            if ((fileAttributes & FILE_BIT_OFFSET) == FILE_BIT_OFFSET)
            {
                directory = true;
            }
            else
            {
                directory = false;
            }

            //stream extension
            lseek(fd, BYTES_PER_ENTRY - bytesReadInEntry, SEEK_CUR); //skip past rest of file dir entry
            bytesReadInCluster += BYTES_PER_ENTRY - bytesReadInEntry;
            bytesReadInEntry = 0;

            if (bytesReadInCluster == (bytesPerSector * sectorsPerCluster)) //go to next cluster
            {
                currCluster = nextCluster(fd, currCluster);
                lseek(fd, findOffsetToCluster(currCluster), SEEK_SET);
                bytesReadInCluster = 0;
            }
            lseek(fd, 3, SEEK_CUR);
            read(fd, &nameLength, 1);
            lseek(fd, 16, SEEK_CUR);
            bytesReadInEntry += 20;
            read(fd, &nextDirCluster, 4);
            bytesReadInEntry += 4;
            lseek(fd, 8, SEEK_CUR);
            bytesReadInEntry += 8;

            bytesReadInEntry = 0;

            bytesReadInCluster += BYTES_PER_ENTRY;

            if (bytesReadInCluster == (bytesPerSector * sectorsPerCluster)) //go to next cluster
            {
                currCluster = nextCluster(fd, currCluster);
                lseek(fd, findOffsetToCluster(currCluster), SEEK_SET);
                bytesReadInCluster = 0;
            }

            lseek(fd, 2 + findOffsetToCluster(currCluster) + bytesReadInCluster, SEEK_SET); //skip first 2 bytes of filename directory
            bytesReadInCluster += 2;
            bytesReadInEntry = 2;

            read(fd, &unicodeString[0], 30);
            bytesReadInCluster += 30;
            bytesReadInEntry = BYTES_PER_ENTRY;

            lseek(fd, findOffsetToCluster(currCluster) + bytesReadInCluster, SEEK_SET);

            //read the secondary entries of the set
            for (int i = 0; i < secondaryCount - 2; i++) // - 2 because first file name and stream extension have been read already
            {
                lseek(fd, 2, SEEK_CUR);
                bytesReadInCluster += 2;
                bytesReadInEntry = 2;
                read(fd, &unicodeString[UNICODE_CHARS_PER_ENTRY * (i + 1)], 30);
                bytesReadInCluster += 30;
                bytesReadInEntry = BYTES_PER_ENTRY;

                if (bytesReadInCluster == (bytesPerSector * sectorsPerCluster)) //go to next cluster
                {
                    currCluster = nextCluster(fd, currCluster);
                    lseek(fd, findOffsetToCluster(currCluster), SEEK_SET);
                    bytesReadInCluster = 0;
                }
            }

            for (int i = 0; i < levels; i++)
            {
                printf("-");
            }
            if (directory)
            {
                printf("Directory: ");
            }
            else
            {
                printf("File: ");
            }
            asciiString = unicode2ascii(unicodeString, nameLength);

            printf("%s\n", asciiString);

            if (directory)
            {
                listRecurse(fdOrig, nextDirCluster, levels + 1);
            }
            free(asciiString);
            lseek(fd, findOffsetToCluster(currCluster) + bytesReadInCluster, SEEK_SET);
        }
        else if (currEntryType == 0)
        {
            done = true;
        }
        if (bytesReadInEntry < BYTES_PER_ENTRY) //skip remaining bytes, go to next entry
        {
            lseek(fd, BYTES_PER_ENTRY - bytesReadInEntry, SEEK_CUR);
            bytesReadInCluster += BYTES_PER_ENTRY - bytesReadInEntry;
        }
        bytesReadInEntry = 0;
        if (bytesReadInCluster == (bytesPerSector * sectorsPerCluster)) //go to next cluster
        {
            currCluster = nextCluster(fd, currCluster);
            lseek(fd, findOffsetToCluster(currCluster), SEEK_SET);
            bytesReadInCluster = 0;
        }
    } //while more files at this level
}

//------------------------------------------------------
// getFile
//
// PURPOSE: Copy the chosen file from the file system to the current directory, one cluster at a time
// INPUT PARAMETERS:
//     file descriptor of exFAT volume, the name of the file to be created, the cluster to look at, the bytes to read for the file (length)
//------------------------------------------------------
void getFile(int fd, char *name, int startCluster, uint64_t length)
{
    int out = open(name, O_RDWR | O_CREAT, PERMISSIONS);
    uint64_t bytesRead = 0;
    int currCluster = startCluster;
    lseek(fd, findOffsetToCluster(currCluster), SEEK_SET);
    uint8_t cluster[bytesPerSector * sectorsPerCluster];
    while (bytesRead != length)
    {
        uint64_t bytesToRead = bytesPerSector * sectorsPerCluster;
        if (length - bytesRead < bytesToRead)
            bytesToRead = length - bytesRead;
        read(fd, cluster, bytesPerSector * sectorsPerCluster);
        write(out, cluster, bytesPerSector * sectorsPerCluster);
        bytesRead += bytesToRead;
        currCluster = nextCluster(fd, currCluster);
        lseek(fd, findOffsetToCluster(currCluster), SEEK_SET);
    }
}

//------------------------------------------------------
// get
//
// PURPOSE: Find where the desired file is stored. Very similar to the listRecurse function above but it will stop searching once the file is found.
// INPUT PARAMETERS:
//     file descriptor of exFAT volume, the cluster to look at, how many levels have been searched (0 to start)
//------------------------------------------------------
void get(int fdOrig, int firstCluster, int levels)
{
    int fd = fdOrig; //we will keep a second file pointer so that when we return from the recusive call the pointer is not affected
    int currCluster = firstCluster;

    //include bytes we 'skip' over in order to track whether to go to next cluster/entry or not
    int bytesReadInCluster = 0;
    int bytesReadInEntry = 0;
    char* asciiString;

    uint8_t nameLength;
    uint64_t length; //of file
    bool done = false;
    uint8_t currEntryType;
    uint8_t secondaryCount;  //to know how many file name directories there is
    uint32_t nextDirCluster; // value from FAT
    bool directory;
    uint16_t unicodeString[MAX_ASCII_STRING_SIZE];


    sectorsPerClus(fd);
    clusterHeapOffset(fd);
    getFatOffset(fd);
    char *myPath = strdup(path);
    char *currentLookUp;
    currentLookUp = strtok(myPath, "/");
    for (int i = 0; i < levels; i++)
    {
        currentLookUp = strtok(NULL, "/");
    }

    lseek(fd, findOffsetToCluster(firstCluster), SEEK_SET);

    while (!done)
    {
        // printf("but here done is set to: %d\n", done);
        read(fd, &currEntryType, 1);
        bytesReadInCluster += 1;
        bytesReadInEntry += 1;
        if (currEntryType == FILE_TYPE_ENTRY)
        {
            read(fd, &secondaryCount, 1);
            bytesReadInEntry += 1;
            bytesReadInCluster += 1;

            lseek(fd, 2, SEEK_CUR); //skip 2 bytes of set checksum
            bytesReadInEntry += 2;
            bytesReadInCluster += 2;
            uint16_t fileAttributes;
            read(fd, &fileAttributes, 2);
            bytesReadInEntry += 2;
            bytesReadInCluster += 2;

            if ((fileAttributes & FILE_BIT_OFFSET) == FILE_BIT_OFFSET)
            {
                directory = true;
            }
            else
            {
                directory = false;
            }

            //stream extension
            lseek(fd, BYTES_PER_ENTRY - bytesReadInEntry, SEEK_CUR); //skip past rest of file dir entry
            bytesReadInCluster += BYTES_PER_ENTRY - bytesReadInEntry;
            bytesReadInEntry = 0;

            if (bytesReadInCluster == (bytesPerSector * sectorsPerCluster)) //go to next cluster
            {
                currCluster = nextCluster(fd, currCluster);
                lseek(fd, findOffsetToCluster(currCluster), SEEK_SET);
                bytesReadInCluster = 0;
            }
            lseek(fd, 3, SEEK_CUR);
            read(fd, &nameLength, 1);
            lseek(fd, 16, SEEK_CUR);
            bytesReadInEntry += 20;
            read(fd, &nextDirCluster, 4);
            bytesReadInEntry += 4;
            read(fd, &length, 8);
            bytesReadInEntry += 8;

            bytesReadInEntry = 0;

            bytesReadInCluster += BYTES_PER_ENTRY;

            if (bytesReadInCluster == (bytesPerSector * sectorsPerCluster)) //go to next cluster
            {
                currCluster = nextCluster(fd, currCluster);
                lseek(fd, findOffsetToCluster(currCluster), SEEK_SET);
                bytesReadInCluster = 0;
            }

            lseek(fd, 2 + findOffsetToCluster(currCluster) + bytesReadInCluster, SEEK_SET); //skip first 2 bytes of filename directory
            bytesReadInCluster += 2;
            bytesReadInEntry = 2;

            read(fd, &unicodeString[0], 30);
            bytesReadInCluster += 30;
            bytesReadInEntry = BYTES_PER_ENTRY;

            lseek(fd, findOffsetToCluster(currCluster) + bytesReadInCluster, SEEK_SET);

            //read the secondary entries of the set
            for (int i = 0; i < secondaryCount - 2; i++) // - 2 because first file name and stream extension have been read already
            {
                lseek(fd, 2, SEEK_CUR);
                bytesReadInCluster += 2;
                bytesReadInEntry = 2;
                read(fd, &unicodeString[UNICODE_CHARS_PER_ENTRY * (i + 1)], 30);
                bytesReadInCluster += 30;
                bytesReadInEntry = BYTES_PER_ENTRY;

                if (bytesReadInCluster == (bytesPerSector * sectorsPerCluster)) //go to next cluster
                {
                    currCluster = nextCluster(fd, currCluster);
                    lseek(fd, findOffsetToCluster(currCluster), SEEK_SET);
                    bytesReadInCluster = 0;
                }
            }

            asciiString = unicode2ascii(unicodeString, nameLength);

            if (directory && strcmp(currentLookUp, asciiString) == 0)
            {
                get(fdOrig, nextDirCluster, levels + 1);
            }
            else if (!directory && strcmp(currentLookUp, asciiString) == 0)
            {
                done = true;
                getFile(fd, currentLookUp, nextDirCluster, length);
            }
            free(asciiString);
            lseek(fd, findOffsetToCluster(currCluster) + bytesReadInCluster, SEEK_SET);
        }
        else if (currEntryType == 0)
        {
            done = true;
        }
        if (bytesReadInEntry < BYTES_PER_ENTRY) //read rest of bytes in entry to go to next entry
        {
            lseek(fd, BYTES_PER_ENTRY - bytesReadInEntry, SEEK_CUR);
            bytesReadInCluster += BYTES_PER_ENTRY - bytesReadInEntry;
        }
        bytesReadInEntry = 0;
        if (bytesReadInCluster == (bytesPerSector * sectorsPerCluster)) //go to next cluster
        {
            currCluster = nextCluster(fd, currCluster);
            lseek(fd, findOffsetToCluster(currCluster), SEEK_SET);
            bytesReadInCluster = 0;
        }

    } //while more files at this level
}

int main(int argc, char *argv[])
{
    assert(argc > 0);
    char *fileName = argv[1];
    char *command = argv[2];
    path = argv[3];
    int fd = open(fileName, O_RDONLY);

    if (strcmp(command, "info") == 0)
    {
        info(fd);
        printf("The volume label is %s\n", volumeLabel);
        printf("Serial Number: 0x%08x or unsigned: %u\n", serialNumber, serialNumber);
        printf("Cluster Size: %d sector(s) or %d bytes\n", sectorsPerCluster, (bytesPerSector * sectorsPerCluster));
        printf("Free Space: %d KB\n", freeSpaceKB);
        free(volumeLabel); //must be freed as the function that created it (unicode2ascii allocates it in heap)
    }
    else if (strcmp(command, "list") == 0)
    {
        getRootDirectory(fd);
        listRecurse(fd, rootDirectory, 0);
    }
    else if (strcmp(command, "get") == 0)
    {
        getRootDirectory(fd);
        get(fd, rootDirectory, 0);
    }

    close(fd);
    return EXIT_SUCCESS;
}
