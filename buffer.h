/**
 * buffer.h
 */

#ifndef BUFFER_H_
#define BUFFER_H_

typedef struct {
	unsigned long totalsize;
	unsigned long currentsize;
	char* buffer;
} Buffer;

Buffer*	bufferInit();
char*	bufferAdd(Buffer* buffer, const char* string, unsigned long size);
void	bufferAllocate(Buffer* buffer, unsigned long size);
char*	bufferDetachBuffer(Buffer* buffer, unsigned int *bufferSize);
void	bufferDestroy(Buffer* buffer);
Buffer*	bufferReset(Buffer* buffer);


#endif /* BUFFER_H_ */
