/*
Simple dump. Dumps the first million pages.

Compiling on FreeBSD:
gcc -O2 -Wall -pedantic --std=c99 -o splitter splitter.c -lbz2 \
	-L/usr/local/lib/ -I/usr/local/include

*/

#define _GNU_SOURCE
#include <bzlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

int help() {
	printf("Syntax: splitter [input] [output]\n");
	return 0;
}


int main(int argc, char** argv) {
	FILE* wiki;
	FILE* newOutput;
	
	int bzError;
	int bytesRead;
	unsigned long totalBytesRead = 0;
	unsigned long documentID = 0;
	BZFILE* compressed;
	char buffer[16384];
	char *current;
	char *potential;
	char *end;
	char shouldStop = 0;
	size_t written;
	
	/* We need input filename and output name for wiki. */
	if (argc != 3) {
		return help();
	}
	
	wiki = fopen(argv[1], "r");
	if (wiki == NULL) {
		perror("Cannot open input file.\n");
		return -1;
	}
	
	newOutput = fopen(argv[2], "w");
	if (newOutput == NULL) {
		perror("Cannot create output file for BOW\n");
		return -1;
	}
	
	compressed = BZ2_bzReadOpen(&bzError, wiki, 0, 0, NULL, 0);
	if (compressed == NULL) {
		perror("Cannot initialize BZIP2\n");
		return -1;
	}
	
	while (!shouldStop) {
		bytesRead = BZ2_bzRead(&bzError, compressed, buffer, sizeof(buffer));
		
		if (bzError != BZ_OK && bzError != BZ_STREAM_END) {
			break;
		}
		
		if (bytesRead == 0) {
			break;
		}
		
		end				= buffer + bytesRead;
		current			= buffer;
		totalBytesRead += bytesRead;
		
		while (current < end) {
			potential = memmem(current, end - current, "</page>", 7);
			if (potential == NULL) {
				/* Cannot find </page> anymore, write out anything left. */
				written = fwrite(current, end - current, 1, newOutput);
				break;
			}
			else {
				potential += 7;
				written = fwrite(current, potential - current, 1, newOutput);
				current = potential;
				++documentID;
				if (documentID == 1000000) {
					written = fwrite("\n</mediawiki>", 13, 1, newOutput);
					shouldStop = 1;
					break;
				}
				
				if (documentID % 10000 == 0) {
					printf("Processed %lu documents.\n", documentID);
				}
			}
		}
		
	}
	
	/* Cleanup any parsing data */
	BZ2_bzReadClose(&bzError, compressed);
	fclose(wiki);
	fclose(newOutput);
	
	printf("Total uncompressed bytes read: %lu, processed documents: %lu\n", totalBytesRead, documentID);
	
	return 0;
}
