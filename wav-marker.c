/** @file wav-labeler.c
 *
 * @author Mike Bucceroni
 * @date 2023-02-10
 * @details program reads a .wav file and a label file as exported by Audacity
 * and creates a new .wav file with embedded cue points and text for each label
 * in a format that works with the podcasting application Forecast
 *
 * Extended from wavecuepoint.c
 * Originally created by Jim McGowan on 2012-11-29
 * Turned into a commandline utility by David Hilowitz on 2016-11-19
 * And modified by Tim Moore on 2022-10-14
 */

#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <ctype.h>

#define WAVE_FORMAT_PCM 0x0001
#define WAVE_FORMAT_IEEE_FLOAT 0x0003

// Some Structs that we use to represent and manipulate Chunks in the Wave files

// The header of a wave file
typedef struct
{
    char chunkID[4];  // Must be "RIFF" (0x52494646)
    char dataSize[4]; // Byte count for the rest of the file (i.e. file length - 8 bytes)
    char riffType[4]; // Must be "WAVE" (0x57415645)
} WaveHeader;

// The format chunk of a wave file
typedef struct
{
    char chunkID[4];                  // String: must be "fmt " (0x666D7420).
    char chunkDataSize[4];            // Unsigned 4-byte little endian int: Byte count for the remainder of the chunk: 16 + extraFormatbytes.
    char compressionCode[2];          // Unsigned 2-byte little endian int
    char numberOfChannels[2];         // Unsigned 2-byte little endian int
    char sampleRate[4];               // Unsigned 4-byte little endian int
    char averageBytesPerSecond[4];    // Unsigned 4-byte little endian int: This value indicates how many bytes of wave data must be streamed to a D/A converter per second in order to play the wave file. This information is useful when determining if data can be streamed from the source fast enough to keep up with playback. = SampleRate * BlockAlign.
    char blockAlign[2];               // Unsigned 2-byte little endian int: The number of bytes per sample slice. This value is not affected by the number of channels and can be calculated with the formula: blockAlign = significantBitsPerSample / 8 * numberOfChannels
    char significantBitsPerSample[2]; // Unsigned 2-byte little endian int
} FormatChunk;

// CuePoint: each individual 'label' in a wave file is represented by a cue point.
typedef struct
{
    char cuePointID[4];        // a unique ID for the Cue Point.
    char playOrderPosition[4]; // Unsigned 4-byte little endian int: If a Playlist chunk is present in the Wave file, this the sample number at which this cue point will occur during playback of the entire play list as defined by the play list's order.  **Otherwise set to same as sample offset??***  Set to 0 when there is no playlist.
    char dataChunkID[4];       // Unsigned 4-byte little endian int: The ID of the chunk containing the sample data that corresponds to this cue point.  If there is no playlist, this should be 'data'.
    char chunkStart[4];        // Unsigned 4-byte little endian int: The byte offset into the Wave List Chunk of the chunk containing the sample that corresponds to this cue point. This is the same chunk described by the Data Chunk ID value. If no Wave List Chunk exists in the Wave file, this value is 0.
    char blockStart[4];        // Unsigned 4-byte little endian int: The byte offset into the "data" or "slnt" Chunk to the start of the block containing the sample. The start of a block is defined as the first byte in uncompressed PCM wave data or the last byte in compressed wave data where decompression can begin to find the value of the corresponding sample value.
    char frameOffset[4];       // Unsigned 4-byte little endian int: The offset into the block (specified by Block Start) for the sample that corresponds to the cue point.
} CuePoint;

// CuePoints are stored in a CueChunk
typedef struct
{
    char chunkID[4];        // String: Must be "cue " (0x63756520).
    char chunkDataSize[4];  // Unsigned 4-byte little endian int: Byte count for the remainder of the chunk: 4 (size of cuePointsCount) + (24 (size of CuePoint struct) * number of CuePoints).
    char cuePointsCount[4]; // Unsigned 4-byte little endian int: Length of cuePoints[].
    CuePoint *cuePoints;
} CueChunk;

// Cue Labels are stored in a ListChunk
typedef struct
{
    char chunkID[4];       // String: Must be "LIST" (0x4C495354).
    char chunkDataSize[4]; // Unsigned 4-byte little endian int: Byte count for the remainder of the chunk: 4 (size of TypeID) + Label chunks.
    char typeID[4];        // String: Must be "adtl" (0x6164746C).
    char *labelChunks;
} ListChunk;

// Some chunks we don't care about the contents and will just copy them from the input file to the output,
// so this struct just stores positions of such chunks in the input file
typedef struct
{
    long startOffset; // in bytes
    size_t size;      // in bytes
} ChunkLocation;

typedef struct
{
    uint32_t locations[500];
    char labels[500][500]; // Hacky Storage for Label strings. 500 characters per label should be plenty
    size_t labelLengths[500];
    uint32_t count;
} LabelInfo;

LabelInfo readLabelFile(FILE *labelFile, FormatChunk formatChunk);

int writeOutputFile(FILE *inputFile, FILE *outputFile, ChunkLocation formatChunkExtraBytes, ChunkLocation dataChunkLocation, int otherChunksCount, ChunkLocation *otherChunkLocations, LabelInfo labelInfo, WaveHeader *waveHeader, FormatChunk *formatChunk, CueChunk cueChunk, ListChunk listChunk, size_t listChunkSize);

// For such chunks that we will copy over from input to output, this function does that in 1MB pieces
int writeChunkLocationFromInputFileToOutputFile(ChunkLocation chunk, FILE *inputFile, FILE *outputFile);

// All data in a Wave file must be little endian.
// These are functions to convert 2- and 4-byte unsigned ints to and from little endian, if needed

enum HostEndiannessType
{
    EndiannessUndefined = 0,
    LittleEndian,
    BigEndian
};

static enum HostEndiannessType HostEndianness = EndiannessUndefined;

enum HostEndiannessType getHostEndianness(void);
uint32_t littleEndianBytesToUInt32(char littleEndianBytes[4]);
void uint32ToLittleEndianBytes(uint32_t uInt32Value, char out_LittleEndianBytes[4]);
uint16_t littleEndianBytesToUInt16(char littleEndianBytes[2]);
void uint16ToLittleEndianBytes(uint16_t uInt16Value, char out_LittleEndianBytes[2]);

uint32_t timeToIndex(float timestamp, FormatChunk formatChunk);

// The main function

static int addLabelsToWaveFile(char *inFilePath, char *labelFilePath, char *outFilePath)
{

    int returnCode = 0;

    // Prepare some variables to hold data read from the input file
    FILE *inputFile = NULL;
    WaveHeader *waveHeader = NULL;
    FormatChunk *formatChunk = NULL;
    ChunkLocation formatChunkExtraBytes = {0, 0};
    ChunkLocation dataChunkLocation = {0, 0};
    const int maxOtherChunks = 256; // How many other chunks can we expect to find?  Who knows! So lets pull 256 out of the air.  That's a nice computery number.
    int otherChunksCount = 0;
    ChunkLocation otherChunkLocations[maxOtherChunks];

    FILE *labelFile = NULL;
    CueChunk cueChunk = {
        .chunkID = {0},
        .chunkDataSize = {0},
        .cuePointsCount = {0},
        .cuePoints = NULL};
    ListChunk listChunk = {
        .chunkID = {0},
        .chunkDataSize = {0},
        .typeID = {0},
        .labelChunks = NULL};
    FILE *outputFile = NULL;

    // Open the Input File
    inputFile = fopen(inFilePath, "rb");
    if (inputFile == NULL)
    {
        fprintf(stderr, "Could not open input file %s\n", inFilePath);
        returnCode = -1;
        goto CleanUpAndExit;
    }

    // Open the Label file
    labelFile = fopen(labelFilePath, "rb");
    if (labelFile == NULL)
    {
        fprintf(stderr, "Could not open label file %s\n", labelFilePath);
        returnCode = -1;
        goto CleanUpAndExit;
    }

    // Get & check the input file header
    fprintf(stdout, "Reading input wave file.\n");

    waveHeader = (WaveHeader *)malloc(sizeof(WaveHeader));
    if (waveHeader == NULL)
    {
        fprintf(stderr, "Memory Allocation Error: Could not allocate memory for Wave File Header\n");
        returnCode = -1;
        goto CleanUpAndExit;
    }

    fread(waveHeader, sizeof(WaveHeader), 1, inputFile);
    if (ferror(inputFile) != 0)
    {
        fprintf(stderr, "Error reading input file %s\n", inFilePath);
        returnCode = -1;
        goto CleanUpAndExit;
    }

    if (strncmp(&(waveHeader->chunkID[0]), "RIFF", 4) != 0)
    {
        fprintf(stderr, "Input file is not a RIFF file\n");
        returnCode = -1;
        goto CleanUpAndExit;
    }

    if (strncmp(&(waveHeader->riffType[0]), "WAVE", 4) != 0)
    {
        fprintf(stderr, "Input file is not a WAVE file\n");
        returnCode = -1;
        goto CleanUpAndExit;
    }

    uint32_t remainingFileSize = littleEndianBytesToUInt32(waveHeader->dataSize) - sizeof(waveHeader->riffType); // dataSize does not counf the chunkID or the dataSize, so remove the riffType size to get the length of the rest of the file.

    if (remainingFileSize <= 0)
    {
        fprintf(stderr, "Input file is an empty WAVE file\n");
        returnCode = -1;
        goto CleanUpAndExit;
    }

    // Start reading in the rest of the wave file
    while (1)
    {
        char nextChunkID[4];

        // Read the ID of the next chunk in the file, and bail if we hit End Of File
        fread(&nextChunkID[0], sizeof(nextChunkID), 1, inputFile);
        if (feof(inputFile))
        {
            break;
        }

        if (ferror(inputFile) != 0)
        {
            fprintf(stderr, "Error reading input file %s\n", inFilePath);
            returnCode = -1;
            goto CleanUpAndExit;
        }

        // See which kind of chunk we have

        if (strncmp(&nextChunkID[0], "fmt ", 4) == 0)
        {
            // We found the format chunk

            formatChunk = (FormatChunk *)malloc(sizeof(FormatChunk));
            if (formatChunk == NULL)
            {
                fprintf(stderr, "Memory Allocation Error: Could not allocate memory for Wave File Format Chunk\n");
                returnCode = -1;
                goto CleanUpAndExit;
            }

            fseek(inputFile, -4, SEEK_CUR);
            fread(formatChunk, sizeof(FormatChunk), 1, inputFile);
            if (ferror(inputFile) != 0)
            {
                fprintf(stderr, "Error reading input file %s\n", inFilePath);
                returnCode = -1;
                goto CleanUpAndExit;
            }

            uint16_t compressionCode = littleEndianBytesToUInt16(formatChunk->compressionCode);
            if (compressionCode != WAVE_FORMAT_PCM && compressionCode != WAVE_FORMAT_IEEE_FLOAT)
            {
                fprintf(stderr, "Compressed audio formats are not supported\n");
                returnCode = -1;
                goto CleanUpAndExit;
            }

            // Note: For compressed audio data there may be extra bytes appended to the format chunk,
            // but as we are only handling uncompressed data we shouldn't encounter them

            // There may or may not be extra data at the end of the fomat chunk.  For uncompressed audio there should be no need, but some files may still have it.
            // if formatChunk.chunkDataSize > 16 (16 = the number of bytes for the format chunk, not counting the 4 byte ID and the chunkDataSize itself) there is extra data
            uint32_t extraFormatBytesCount = littleEndianBytesToUInt32(formatChunk->chunkDataSize) - 16;
            if (extraFormatBytesCount > 0)
            {
                formatChunkExtraBytes.startOffset = ftell(inputFile);
                formatChunkExtraBytes.size = extraFormatBytesCount;
                fseek(inputFile, extraFormatBytesCount, SEEK_CUR);
                if (extraFormatBytesCount % 2 != 0)
                {
                    fseek(inputFile, 1, SEEK_CUR);
                }
            }

            printf("Got Format Chunk\n");
        }

        else if (strncmp(&nextChunkID[0], "data", 4) == 0)
        {
            // We found the data chunk
            dataChunkLocation.startOffset = ftell(inputFile) - (long)sizeof(nextChunkID);

            // The next 4 bytes are the chunk data size - the size of the sample data
            char sampleDataSizeBytes[4];
            fread(sampleDataSizeBytes, sizeof(char), 4, inputFile);
            if (ferror(inputFile) != 0)
            {
                fprintf(stderr, "Error reading input file %s\n", inFilePath);
                returnCode = -1;
                goto CleanUpAndExit;
            }
            uint32_t sampleDataSize = littleEndianBytesToUInt32(sampleDataSizeBytes);

            dataChunkLocation.size = sizeof(nextChunkID) + sizeof(sampleDataSizeBytes) + sampleDataSize;

            // Skip to the end of the chunk.  Chunks must be aligned to 2 byte boundaries, but any padding at the end of a chunk is not included in the chunkDataSize
            fseek(inputFile, sampleDataSize, SEEK_CUR);
            if (sampleDataSize % 2 != 0)
            {
                fseek(inputFile, 1, SEEK_CUR);
            }

            printf("Got Data Chunk\n");
        }

        else if (strncmp(&nextChunkID[0], "cue ", 4) == 0)
        {
            // We found an existing Cue Chunk

            char cueChunkDataSizeBytes[4];
            fread(cueChunkDataSizeBytes, sizeof(char), 4, inputFile);
            if (ferror(inputFile) != 0)
            {
                fprintf(stderr, "Error reading input file %s\n", inFilePath);
                returnCode = -1;
                goto CleanUpAndExit;
            }
            uint32_t cueChunkDataSize = littleEndianBytesToUInt32(cueChunkDataSizeBytes);

            // Skip over the chunk's data, and any padding byte
            fseek(inputFile, cueChunkDataSize, SEEK_CUR);
            if (cueChunkDataSize % 2 != 0)
            {
                fseek(inputFile, 1, SEEK_CUR);
            }

            printf("Found Existing Cue Chunk\n");
        }

        else
        {
            bool isadtl = false;

            if ((strncmp(&nextChunkID[0], "LIST", 4) == 0))
            {
                char listTypeID[4];

                char chunkDataSizeBytes[4] = {0};
                fread(chunkDataSizeBytes, sizeof(char), 4, inputFile);
                if (ferror(inputFile) != 0)
                {
                    fprintf(stderr, "Error reading input file %s\n", inFilePath);
                    returnCode = -1;
                    goto CleanUpAndExit;
                }
                uint32_t chunkDataSize = littleEndianBytesToUInt32(chunkDataSizeBytes);

                fread(&listTypeID[0], sizeof(listTypeID), 1, inputFile);
                if (feof(inputFile))
                {
                    break;
                }

                if (ferror(inputFile) != 0)
                {
                    fprintf(stderr, "Error reading input file %s\n", inFilePath);
                    returnCode = -1;
                    goto CleanUpAndExit;
                }

                if ((strncmp(&listTypeID[0], "adtl", 4) == 0))
                {
                    isadtl = true;
                    printf("Found Existing Label Chunk\n");
                    // Skip over the chunk's data, and any padding byte
                    fseek(inputFile, chunkDataSize - (sizeof(char) * 4), SEEK_CUR);
                    if (chunkDataSize % 2 != 0)
                    {
                        fseek(inputFile, 1, SEEK_CUR);
                    }
                }
                else
                {
                    // if its not an adtl type go back and save chunk info
                    fseek(inputFile, (-1 * (long)((sizeof(char) * 4) + sizeof(listTypeID))), SEEK_CUR);
                }
            }

            if (!isadtl)
            {
                // We have found a chunk type that we are not going to work with.  Just note the location so we can copy it to the output file later

                if (otherChunksCount >= maxOtherChunks)
                {
                    fprintf(stderr, "Input file has more chunks than the maximum supported by this program (%d)\n", maxOtherChunks);
                    returnCode = -1;
                    goto CleanUpAndExit;
                }

                otherChunkLocations[otherChunksCount].startOffset = ftell(inputFile) - (long)sizeof(nextChunkID);

                char chunkDataSizeBytes[4] = {0};
                fread(chunkDataSizeBytes, sizeof(char), 4, inputFile);
                if (ferror(inputFile) != 0)
                {
                    fprintf(stderr, "Error reading input file %s\n", inFilePath);
                    returnCode = -1;
                    goto CleanUpAndExit;
                }
                uint32_t chunkDataSize = littleEndianBytesToUInt32(chunkDataSizeBytes);

                otherChunkLocations[otherChunksCount].size = sizeof(nextChunkID) + sizeof(chunkDataSizeBytes) + chunkDataSize;

                // Skip over the chunk's data, and any padding byte
                fseek(inputFile, chunkDataSize, SEEK_CUR);
                if (chunkDataSize % 2 != 0)
                {
                    fseek(inputFile, 1, SEEK_CUR);
                }

                otherChunksCount++;

                fprintf(stdout, "Found chunk type \'%c%c%c%c\', size: %d bytes\n", nextChunkID[0], nextChunkID[1], nextChunkID[2], nextChunkID[3], chunkDataSize);
            }
        }
    }

    // Did we get enough data from the input file to proceed?

    if ((formatChunk == NULL) || (dataChunkLocation.size == 0))
    {
        fprintf(stderr, "Input file did not contain any format data or did not contain any sample data\n");
        returnCode = -1;
        goto CleanUpAndExit;
    }

    // Read in the Label File
    fprintf(stdout, "Reading label file.\n");

    LabelInfo labelInfo = readLabelFile(labelFile, *formatChunk);

    // Did we get any LabelInfo?
    if (labelInfo.count < 1)
    {
        fprintf(stderr, "Did not find any cue point locations in the label file\n");
        returnCode = -1;
        goto CleanUpAndExit;
    }

    fprintf(stdout, "Read %d cue locations from label file.\nPreparing new cue chunk.\n", labelInfo.count);

    // Create CuePointStructs for each cue location
    cueChunk.cuePoints = malloc(sizeof(CuePoint) * labelInfo.count);
    if (cueChunk.cuePoints == NULL)
    {
        fprintf(stderr, "Memory Allocation Error: Could not allocate memory for Cue Points data\n");
        returnCode = -1;
        goto CleanUpAndExit;
    }

    fprintf(stdout, "Preparing new label chunk.\n");

    size_t listChunkSize = 0;

    // calculate size of List Chunk
    for (uint32_t i = 0; i < labelInfo.count; i++)
    {
        // chunkID (4) + Chunk Data Size (4) + Cuepoint ID (4) + Text
        listChunkSize += (12 + labelInfo.labelLengths[i]);
        // add padding byte
        if ((labelInfo.labelLengths[i] % 2) != 0)
        {
            listChunkSize++;
        }
    }

    listChunk.labelChunks = malloc(sizeof(char) * listChunkSize);
    if (listChunk.labelChunks == NULL)
    {
        fprintf(stderr, "Memory Allocation Error: Could not allocate memory for Label data\n");
        returnCode = -1;
        goto CleanUpAndExit;
    }

    size_t listChunkIndex = 0;

    for (uint32_t i = 0; i < labelInfo.count; i++)
    {
        // Cues
        uint32ToLittleEndianBytes(i + 1, cueChunk.cuePoints[i].cuePointID);
        uint32ToLittleEndianBytes(labelInfo.locations[i], cueChunk.cuePoints[i].playOrderPosition);
        cueChunk.cuePoints[i].dataChunkID[0] = 'd';
        cueChunk.cuePoints[i].dataChunkID[1] = 'a';
        cueChunk.cuePoints[i].dataChunkID[2] = 't';
        cueChunk.cuePoints[i].dataChunkID[3] = 'a';
        uint32ToLittleEndianBytes(0, cueChunk.cuePoints[i].chunkStart);
        uint32ToLittleEndianBytes(0, cueChunk.cuePoints[i].blockStart);
        uint32ToLittleEndianBytes(labelInfo.locations[i], cueChunk.cuePoints[i].frameOffset);

        // Labels
        listChunk.labelChunks[listChunkIndex++] = 'l';
        listChunk.labelChunks[listChunkIndex++] = 'a';
        listChunk.labelChunks[listChunkIndex++] = 'b';
        listChunk.labelChunks[listChunkIndex++] = 'l';
        char labelLength[4];
        uint32ToLittleEndianBytes(labelInfo.labelLengths[i] + 4, labelLength);
        listChunk.labelChunks[listChunkIndex++] = labelLength[0];
        listChunk.labelChunks[listChunkIndex++] = labelLength[1];
        listChunk.labelChunks[listChunkIndex++] = labelLength[2];
        listChunk.labelChunks[listChunkIndex++] = labelLength[3];
        listChunk.labelChunks[listChunkIndex++] = cueChunk.cuePoints[i].cuePointID[0];
        listChunk.labelChunks[listChunkIndex++] = cueChunk.cuePoints[i].cuePointID[1];
        listChunk.labelChunks[listChunkIndex++] = cueChunk.cuePoints[i].cuePointID[2];
        listChunk.labelChunks[listChunkIndex++] = cueChunk.cuePoints[i].cuePointID[3];
        for (uint32_t j = 0; j < labelInfo.labelLengths[i]; j++)
        {
            listChunk.labelChunks[listChunkIndex++] = labelInfo.labels[i][j];
        }
        // add padding if odd length
        if ((labelInfo.labelLengths[i] % 2) != 0)
        {
            listChunk.labelChunks[listChunkIndex++] = 0;
        }
    }

    // Populate the CueChunk Struct
    cueChunk.chunkID[0] = 'c';
    cueChunk.chunkID[1] = 'u';
    cueChunk.chunkID[2] = 'e';
    cueChunk.chunkID[3] = ' ';
    uint32ToLittleEndianBytes(4 + (sizeof(CuePoint) * labelInfo.count), cueChunk.chunkDataSize); // See struct definition
    uint32ToLittleEndianBytes(labelInfo.count, cueChunk.cuePointsCount);

    listChunk.chunkID[0] = 'L';
    listChunk.chunkID[1] = 'I';
    listChunk.chunkID[2] = 'S';
    listChunk.chunkID[3] = 'T';
    uint32ToLittleEndianBytes(4 + (sizeof(char) * listChunkSize), listChunk.chunkDataSize);
    listChunk.typeID[0] = 'a';
    listChunk.typeID[1] = 'd';
    listChunk.typeID[2] = 't';
    listChunk.typeID[3] = 'l';

    // Open the output file for writing
    outputFile = fopen(outFilePath, "w+b");
    if (outputFile == NULL)
    {
        fprintf(stderr, "Could not open output file %s\nError: %d\n", outFilePath, errno);
        returnCode = -1;
        goto CleanUpAndExit;
    }

    returnCode = writeOutputFile(inputFile, outputFile, formatChunkExtraBytes, dataChunkLocation, otherChunksCount, otherChunkLocations, labelInfo, waveHeader, formatChunk, cueChunk, listChunk, listChunkSize);
    if (returnCode < 0)
    {
        goto CleanUpAndExit;
    }

    printf("Finished.\n");

CleanUpAndExit:

    if (inputFile != NULL)
        fclose(inputFile);
    if (waveHeader != NULL)
        free(waveHeader);
    if (formatChunk != NULL)
        free(formatChunk);
    if (labelFile != NULL)
        fclose(labelFile);
    if (cueChunk.cuePoints != NULL)
        free(cueChunk.cuePoints);
    if (listChunk.labelChunks != NULL)
        free(listChunk.labelChunks);
    if (outputFile != NULL)
        fclose(outputFile);

    return returnCode;
}

LabelInfo readLabelFile(FILE *labelFile, FormatChunk formatChunk)
{
    // The label file should follow the standard format exported by audacity "startTime(sec) \t endTime(sec) \t Label \n" endTime will be ignored
    LabelInfo labelInfo = {
        .locations = {0},
        .labels = {0},
        .labelLengths = {0},
        .count = 0};

    int lineNumber = 1;

    while (!feof(labelFile))
    {
        float startTime = 0.0;
        char labelString[500]; // Labels are unlikely to exceed 500 characters.

        if (fscanf(labelFile, "%f%*f%*c%[^\r\n]%*c", &startTime, &labelString) != 2)
        {

            fprintf(stderr, "Line %d in label file is not formatted correctly it should be \"startTime(sec) \\t endTime(sec) \\t Label \\n\"", lineNumber);
        }
        else
        {
            if (startTime <= 48660)
            {
                labelInfo.locations[labelInfo.count] = timeToIndex(startTime, formatChunk);
                strcpy(labelInfo.labels[labelInfo.count], labelString);
                labelInfo.labelLengths[labelInfo.count] = strlen(labelString) + 1;
                labelInfo.count++;
            }
            else
            {
                fprintf(stderr, "Line %d in label file contains a value larger than the max possible wav length (48,660.0 seconds)\n", lineNumber);
            }
        }

        int nextChar = fgetc(labelFile);

        if (nextChar == '\r')
        {
            // This is a Classic Mac line ending '\r' or the start of a Windows line ending '\r\n'
            // If this is the start of a '\r\n', gobble up the '\n' too
            int peekAheadChar = fgetc(labelFile);
            if ((peekAheadChar != EOF) && (peekAheadChar != '\n'))
            {
                ungetc(peekAheadChar, labelFile);
            }
        }

        nextChar = fgetc(labelFile);
        if (!feof(labelFile))
        {
            if (nextChar == '\r')
            {
                // This is a Classic Mac line ending '\r' or the start of a Windows line ending '\r\n'
                // If this is the start of a '\r\n', gobble up the '\n' too
                int peekAheadChar = fgetc(labelFile);
                if ((peekAheadChar != EOF) && (peekAheadChar != '\n'))
                {
                    ungetc(peekAheadChar, labelFile);
                }
            }
            else
            {
                ungetc(nextChar, labelFile);
            }
        }

        lineNumber++;
    }

    return labelInfo;
}

int writeOutputFile(FILE *inputFile, FILE *outputFile, ChunkLocation formatChunkExtraBytes, ChunkLocation dataChunkLocation, int otherChunksCount, ChunkLocation *otherChunkLocations, LabelInfo labelInfo, WaveHeader *waveHeader, FormatChunk *formatChunk, CueChunk cueChunk, ListChunk listChunk, size_t listChunkSize)
{
    fprintf(stdout, "Writing output file.\n");

    // Update the file header chunk to have the new data size
    uint32_t fileDataSize = 0;
    fileDataSize += 4; // the 4 bytes for the Riff Type "WAVE"
    fileDataSize += sizeof(FormatChunk);
    fileDataSize += formatChunkExtraBytes.size;
    if (formatChunkExtraBytes.size % 2 != 0)
    {
        fileDataSize++; // Padding byte for 2byte alignment
    }

    fileDataSize += dataChunkLocation.size;
    if (dataChunkLocation.size % 2 != 0)
    {
        fileDataSize++;
    }

    for (int i = 0; i < otherChunksCount; i++)
    {
        fileDataSize += otherChunkLocations[i].size;
        if (otherChunkLocations[i].size % 2 != 0)
        {
            fileDataSize++;
        }
    }
    fileDataSize += 4; // 4 bytes for CueChunk ID "cue "
    fileDataSize += 4; // UInt32 for CueChunk.chunkDataSize
    fileDataSize += 4; // UInt32 for CueChunk.cuePointsCount
    fileDataSize += (sizeof(CuePoint) * labelInfo.count);

    fileDataSize += 4; // 4 bytes for ListChunk ID "LIST"
    fileDataSize += 4; // UInt32 for ListChunk.chunkDataSize
    fileDataSize += 4; // 4 bytes for TypeID "adtl"
    fileDataSize += (sizeof(char) * listChunkSize);

    uint32ToLittleEndianBytes(fileDataSize, waveHeader->dataSize);

    // Write out the header to the new file
    if (fwrite(waveHeader, sizeof(*waveHeader), 1, outputFile) < 1)
    {
        fprintf(stderr, "Error writing header to output file.\n");
        return -1;
    }

    // Write out the format chunk
    if (fwrite(formatChunk, sizeof(FormatChunk), 1, outputFile) < 1)
    {
        fprintf(stderr, "Error writing format chunk to output file.\n");
        return -1;
    }
    else if (formatChunkExtraBytes.size > 0)
    {
        if (writeChunkLocationFromInputFileToOutputFile(formatChunkExtraBytes, inputFile, outputFile) < 0)
        {
            return -1;
        }
        if (formatChunkExtraBytes.size % 2 != 0)
        {
            if (fwrite("\0", sizeof(char), 1, outputFile) < 1)
            {
                fprintf(stderr, "Error writing padding character to output file.\n");
                return -1;
            }
        }
    }

    // Write out the data chunk
    if (writeChunkLocationFromInputFileToOutputFile(dataChunkLocation, inputFile, outputFile) < 0)
    {
        return -1;
    }
    if (dataChunkLocation.size % 2 != 0)
    {
        if (fwrite("\0", sizeof(char), 1, outputFile) < 1)
        {
            fprintf(stderr, "Error writing padding character to output file.\n");
            return -1;
        }
    }

    // Write out the start of new Cue Chunk: chunkID, dataSize and cuePointsCount
    if (fwrite(&cueChunk, sizeof(cueChunk.chunkID) + sizeof(cueChunk.chunkDataSize) + sizeof(cueChunk.cuePointsCount), 1, outputFile) < 1)
    {
        fprintf(stderr, "Error writing cue chunk header to output file.\n");
        return -1;
    }

    // Write out the Cue Points
    for (uint32_t i = 0; i < littleEndianBytesToUInt32(cueChunk.cuePointsCount); i++)
    {
        if (fwrite(&(cueChunk.cuePoints[i]), sizeof(CuePoint), 1, outputFile) < 1)
        {
            fprintf(stderr, "Error writing cue point to output file.\n");
            return -1;
        }
    }

    // Write out adtl chunk

    // Write out the start of new List Chunk: chunkID, dataSize and TypeID
    if (fwrite(&listChunk, sizeof(listChunk.chunkID) + sizeof(listChunk.chunkDataSize) + sizeof(listChunk.typeID), 1, outputFile) < 1)
    {
        fprintf(stderr, "Error writing adtl chunk header to output file.\n");
        return -1;
    }

    // Write out the Labels
    if (fwrite(&listChunk.labelChunks[0], listChunkSize, 1, outputFile) < 1)
    {
        fprintf(stderr, "Error writing labels to output file.\n");
        return -1;
    }

    if ((listChunkSize % 2) != 0)
    {
        if (fwrite("\0", sizeof(char), 1, outputFile) < 1)
        {
            fprintf(stderr, "Error writing padding character to output file.\n");
            return -1;
        }
    }

    // Write out the other chunks from the input file
    for (int i = 0; i < otherChunksCount; i++)
    {
        if (writeChunkLocationFromInputFileToOutputFile(otherChunkLocations[i], inputFile, outputFile) < 0)
        {
            return -1;
        }
        if (otherChunkLocations[i].size % 2 != 0)
        {
            if (fwrite("\0", sizeof(char), 1, outputFile) < 1)
            {
                fprintf(stderr, "Error writing padding character to output file.\n");
                return -1;
            }
        }
    }

    return 0;
}

int writeChunkLocationFromInputFileToOutputFile(ChunkLocation chunk, FILE *inputFile, FILE *outputFile)
{
    // note the position of the input file to restore later
    long inputFileOrigLocation = ftell(inputFile);

    if (fseek(inputFile, chunk.startOffset, SEEK_SET) < 0)
    {
        fprintf(stderr, "Error: could not seek input file to location %ld", chunk.startOffset);
        return -1;
    }

    size_t remainingBytesToWrite = chunk.size;
    while (remainingBytesToWrite >= 1024)
    {
        char buffer[1024];

        fread(buffer, sizeof(char), 1024, inputFile);
        if (ferror(inputFile) != 0)
        {
            fprintf(stderr, "Copy chunk: Error reading input file");
            fseek(inputFile, inputFileOrigLocation, SEEK_SET);
            return -1;
        }

        if (fwrite(buffer, sizeof(char), 1024, outputFile) < 1)
        {
            fprintf(stderr, "Copy chunk: Error writing output file");
            fseek(inputFile, inputFileOrigLocation, SEEK_SET);
            return -1;
        }
        remainingBytesToWrite -= 1024;
    }

    if (remainingBytesToWrite > 0)
    {
        char buffer[remainingBytesToWrite];

        fread(buffer, sizeof(char), remainingBytesToWrite, inputFile);
        if (ferror(inputFile) != 0)
        {
            fprintf(stderr, "Copy chunk: Error reading input file");
            fseek(inputFile, inputFileOrigLocation, SEEK_SET);
            return -1;
        }

        if (fwrite(buffer, sizeof(char), remainingBytesToWrite, outputFile) < 1)
        {
            fprintf(stderr, "Copy chunk: Error writing output file");
            fseek(inputFile, inputFileOrigLocation, SEEK_SET);
            return -1;
        }
    }

    return 0;
}

enum HostEndiannessType getHostEndianness()
{
    int i = 1;
    char *p = (char *)&i;

    if (p[0] == 1)
        return LittleEndian;
    else
        return BigEndian;
}

uint32_t littleEndianBytesToUInt32(char littleEndianBytes[4])
{
    if (HostEndianness == EndiannessUndefined)
    {
        HostEndianness = getHostEndianness();
    }

    uint32_t uInt32Value;
    char *uintValueBytes = (char *)&uInt32Value;

    if (HostEndianness == LittleEndian)
    {
        uintValueBytes[0] = littleEndianBytes[0];
        uintValueBytes[1] = littleEndianBytes[1];
        uintValueBytes[2] = littleEndianBytes[2];
        uintValueBytes[3] = littleEndianBytes[3];
    }
    else
    {
        uintValueBytes[0] = littleEndianBytes[3];
        uintValueBytes[1] = littleEndianBytes[2];
        uintValueBytes[2] = littleEndianBytes[1];
        uintValueBytes[3] = littleEndianBytes[0];
    }

    return uInt32Value;
}

void uint32ToLittleEndianBytes(uint32_t uInt32Value, char out_LittleEndianBytes[4])
{
    if (HostEndianness == EndiannessUndefined)
    {
        HostEndianness = getHostEndianness();
    }

    char *uintValueBytes = (char *)&uInt32Value;

    if (HostEndianness == LittleEndian)
    {
        out_LittleEndianBytes[0] = uintValueBytes[0];
        out_LittleEndianBytes[1] = uintValueBytes[1];
        out_LittleEndianBytes[2] = uintValueBytes[2];
        out_LittleEndianBytes[3] = uintValueBytes[3];
    }
    else
    {
        out_LittleEndianBytes[0] = uintValueBytes[3];
        out_LittleEndianBytes[1] = uintValueBytes[2];
        out_LittleEndianBytes[2] = uintValueBytes[1];
        out_LittleEndianBytes[3] = uintValueBytes[0];
    }
}

uint16_t littleEndianBytesToUInt16(char littleEndianBytes[2])
{
    if (HostEndianness == EndiannessUndefined)
    {
        HostEndianness = getHostEndianness();
    }

    uint16_t uInt16Value;
    char *uintValueBytes = (char *)&uInt16Value;

    if (HostEndianness == LittleEndian)
    {
        uintValueBytes[0] = littleEndianBytes[0];
        uintValueBytes[1] = littleEndianBytes[1];
    }
    else
    {
        uintValueBytes[0] = littleEndianBytes[1];
        uintValueBytes[1] = littleEndianBytes[0];
    }

    return uInt16Value;
}

void uint16ToLittleEndianBytes(uint16_t uInt16Value, char out_LittleEndianBytes[2])
{
    if (HostEndianness == EndiannessUndefined)
    {
        HostEndianness = getHostEndianness();
    }

    char *uintValueBytes = (char *)&uInt16Value;

    if (HostEndianness == LittleEndian)
    {
        out_LittleEndianBytes[0] = uintValueBytes[0];
        out_LittleEndianBytes[1] = uintValueBytes[1];
    }
    else
    {
        out_LittleEndianBytes[0] = uintValueBytes[1];
        out_LittleEndianBytes[1] = uintValueBytes[0];
    }
}

uint32_t timeToIndex(float timestamp, FormatChunk formatChunk)
{
    uint32_t index;
    uint32_t sampleRate = littleEndianBytesToUInt32(formatChunk.sampleRate);
    uint16_t numberOfChannels = littleEndianBytesToUInt16(formatChunk.numberOfChannels);
    index = timestamp * sampleRate;
    return index;
}

int main(int argc, char **argv)
{
    char *inFilePath = NULL;
    char *labelFilePath = NULL;
    char *outFilePath = NULL;

    if (argc != 4)
    {
        printf("Usage: wav-marker WAVFILE labelFILE OUTPUTFILE\n%d", argc);
        return 1;
    }

    inFilePath = argv[1];
    labelFilePath = argv[2];
    outFilePath = argv[3];

    printf("inFilePath = %s, labelFilePath = %s, outFilePath = %s\n",
           inFilePath, labelFilePath, outFilePath);

    return addLabelsToWaveFile(inFilePath, labelFilePath, outFilePath);
}
