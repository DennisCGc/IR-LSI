/**
 * buffer.c
 */

#include "buffer.h"
#include <stdlib.h>
#include <string.h>

Buffer *bufferInit() {
	char* buffer;
	Buffer* bufferStruct;

	buffer						= (char *) malloc(1024);
	bufferStruct				= (Buffer *) malloc(sizeof(Buffer));

	bufferStruct->totalsize		= 1024;
	bufferStruct->currentsize	= 0;
	bufferStruct->buffer		= buffer;
	
	return bufferStruct;
}

void bufferAllocate(Buffer* buffer, unsigned long size) {
	if ((size + buffer->currentsize) > buffer->totalsize) {
		/* Reallocate at least the required memory needed. */
		unsigned long memoryNeeded = size+buffer->currentsize-buffer->totalsize;

		/* Round it to the upper 1K. */
		memoryNeeded += (memoryNeeded % 1024) ? 1024 : 0;
		memoryNeeded = memoryNeeded & ~1023;
		buffer->totalsize += memoryNeeded;

		/* Reallocate the buffer to totalsize+memoryNeeded. */
		buffer->buffer = (char *) realloc(buffer->buffer, buffer->totalsize);
	}
}

char* bufferAdd(Buffer* buffer, const char* string, unsigned long size) {

	if (size == 0) {
		return buffer->buffer;
	}

	bufferAllocate(buffer, size);

	char* returnAddress = buffer->buffer+buffer->currentsize;
	memcpy(buffer->buffer+buffer->currentsize, string, size);
	
	buffer->currentsize += size;

	return returnAddress;
}

Buffer* bufferReset(Buffer* buffer) {
	if (buffer->totalsize != 1024) {
		free(buffer->buffer);
		buffer->buffer		= (char*) malloc(1024);
		buffer->totalsize	= 1024;
	}

	buffer->currentsize	= 0;

	return buffer;
}

void bufferDestroy(Buffer *buffer) {
	free(buffer->buffer);
	free(buffer);
}
