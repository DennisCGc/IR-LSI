/*
Simple tokenizer for wikipedia dump
- Uses bzip2 library for decompression on the fly
- Uses libxml2 xml reader to parse the document. Specifically, it uses the xmlReader
	API in order to make sure we use it from a streaming source.
- Uses klib/khash for hash maps.
- The rest is just "hacked" up together in order to make it work :-)

Compiling on FreeBSD:
gcc -O2 -Wall -pedantic --std=c99 -o tokenizer tokenizer.c -lbz2 \
	-lxml2 -L/usr/local/lib/ -I/usr/local/include/libxml2/ -I/usr/local/include

*/

#include <bzlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <libxml/xmlreader.h>
#include "khash.h"

typedef struct {
	unsigned long id;
	char* token;
	unsigned long occurence;
} TokenDesc;

KHASH_MAP_INIT_STR(Tokens, TokenDesc*)
khash_t(Tokens)* tokens;

long totalBytesRead = 0;
long amountTokens = 0;
long documentID = 0;

typedef struct {
	const xmlChar* title;
	const xmlChar* content;
} Page;

/* XML parsing functions */
void processTitle(xmlTextReader* reader, Page* page) {
	const xmlChar* nodeName;
	int type;
	while (xmlTextReaderRead(reader) == 1) {
		type		= xmlTextReaderNodeType(reader);
		
		/* Start of text? */
		if (type == 3) {
			page->title = xmlTextReaderValue(reader);
		}
		/* End element? */
		else if (type == 15) {
			nodeName = xmlTextReaderConstName(reader);
			if (xmlStrcmp(nodeName, (const xmlChar*) "title") == 0) {
				return;
			}
		}
	}	
}

void processText(xmlTextReader* reader, Page* page) {
	const xmlChar* nodeName;
	int type;
	while (xmlTextReaderRead(reader) == 1) {
		type		= xmlTextReaderNodeType(reader);
		
		/* Start of text? */
		if (type == 3) {
			page->content = xmlTextReaderValue(reader);
		}
		/* End element? */
		else if (type == 15) {
			nodeName = xmlTextReaderConstName(reader);
			if (xmlStrcmp(nodeName, (const xmlChar*) "text") == 0) {
				return;
			}
		}
	}	
}

void processRevision(xmlTextReader* reader, Page* page) {
	const xmlChar* nodeName;
	int type;
	while (xmlTextReaderRead(reader) == 1) {
		type		= xmlTextReaderNodeType(reader);
		nodeName	= xmlTextReaderConstName(reader);
		
		/* Start of an element? */
		if (type == 1) {
			if (xmlStrcmp(nodeName, (const xmlChar*) "text") == 0) {
				processText(reader, page);
			}
		}
		/* End element? */
		else if (type == 15) {
			if (xmlStrcmp(nodeName, (const xmlChar*) "revision") == 0) {
				return;
			}
		}
	}
}

/* Markup deletion functions. */

/*Loosely adapted from wikicorpus.py */
char* removeTemplate(char* source, char* end) {
	char* destination = source;
	int open = 0;
	
	while (source < end) {
		
		if (open == 0) {
			if (*source == '{' && (source + 1 < end) && *(source + 1) == '{') {
				open += 2;
				++source;
			}
			else {
				*destination++ = *source;
			}
		}
		else {
			if (*source == '}') {
				--open;
			}
			else if (*source == '{') {
				++open;
			}
		}
		
		++source;
	}
	
	*destination++ = '\0';
	
	return destination;
}

char* removeComments(char* source, char* end) {
	char* destination = source;
	char* found;
	int bytesToCopy = 0;
	
	while (source < end) {
		found = strstr(source, "<!--");
		
		if (found == NULL) {
			if (destination == source) {
				return end;
			}
			
			bytesToCopy = end - source;
			bcopy(source, destination, bytesToCopy);
			destination += bytesToCopy;
			break;
		}
		else {
			bytesToCopy = found - source;
			bcopy(source, destination, bytesToCopy);
			destination += bytesToCopy;
			
			found = strstr(found + 4, "-->");
			if (found == NULL) {
				/* Unmatched, should not happen. */
				break;
			}
			
			source = found + 3;
		}
	}
	
	*destination++ = '\0';
	return destination;
}

char* removeTags(char* source, char* end) {
	char* destination = source;
	char* start = NULL;
	char open = 0;
	char ignore = 0;
	char metaText = 0;
	char c;
	int bytesToCopy = 0;
	
	while (source < end) {
		
		c = *source;
		
		if (!open) {
			if (c == '[' && (source + 1 < end) && *(source + 1) == '[') {
				open = 1;
				source += 2;
				start = source;
			}
			else {
				*destination++ = *source++;
			}
		}
		else {
			if (c == '[' && (source + 1 < end) && *(source + 1) == '[') {
				end = removeTags(source, end);
			}
			else if (c == ']' && (source + 1 < end) && *(source + 1) == ']') {
				if (!ignore) {
					bytesToCopy = source - start;
					bcopy(start, destination, bytesToCopy);
					destination += bytesToCopy;
				}
				
				open		= 0;
				source		+= 2;
				
				/* Cleanup any state from parsing [[]]s */
				metaText	= 0;
				ignore		= 0;
			}
			/* Don't ignore any meta text. This approach has the advantage that it selects only the right most meta text,
			potentially ignoring any other meta data such as thumb. */ 
			else if (c == '|') {
				ignore = 0;
				metaText = 1;
				start = ++source;
			}
			/* Any [[foobar: is ignored. */
			else if (c == ':' && !metaText) {
				ignore = 1;
				++source;
			}
			else {
				++source;
			}
		}
		
	}
	
	*destination++ = '\0';
	return destination;
}

/* Tries to remove any hyperlinks. Does not remove links in the format: http://example.com, i.e.
it only removes [http://example.com] style links, while keeping any description of the link. */
char* removeHyperlinks(char* source, char* end) {
	char* destination = source;
	char* found = NULL;
	int bytesToCopy = 0;
	char* metaText = NULL;
	
	while (source < end) {
		found = strchr(source, '[');
		if (found == NULL) {
			if (destination == source) {
				return end;
			}
			
			bytesToCopy = end - source;
			bcopy(source, destination, bytesToCopy);
			destination += bytesToCopy;
			break;
		}
		else {
			bytesToCopy = found - source;
			bcopy(source, destination, bytesToCopy);
			destination += bytesToCopy;
			source = found + 1;
			
			while (source < end) {
				if (*source == ']') {
					if (metaText != NULL) {
						bytesToCopy = source - metaText;
						bcopy(metaText, destination, bytesToCopy);
						destination += bytesToCopy;
						metaText = NULL;
					}
					++source;
					break;
				}
				
				if (metaText == NULL && *source == ' ') {
					metaText = source + 1;
				}
				
				++source;
			}
			
		}
		
	}
	
	*destination++ = '\0';
	return destination;	
}

/* Very simple HTML remover. */
char* removeHTMLTags(char* source, char* end) {
	char *destination = source;
	char* found = NULL;
	char ignoreMath = 0;
	int bytesToCopy = 0;

	while (source < end) {
		found = strchr(source, '<');
		if (found == NULL) {
			if (destination == source) {
				return end;
			}
			
			bytesToCopy = end - source;
			bcopy(source, destination, bytesToCopy);
			destination += bytesToCopy;
			break;
		}
		else {
			bytesToCopy = found - source;
			bcopy(source, destination, bytesToCopy);
			destination += bytesToCopy;
			source = found + 1;
			
			if (strncasecmp(source, "math", 4) == 0) {
				ignoreMath = 1;
			}
			
			found = strchr(source, '>');
			if (found == NULL) {
				break;
			}
			
			source = found + 1;
			if (ignoreMath) {
				found = strstr(source, "</math>");
				if (found == NULL) {
					break;
				}
				
				source = found + 8;
			}
		}
	}
	
	return destination;
}


void toLower(char* s) {
	while (*s != '\0') {
		*s = tolower(*s);
		++s;		
	}
}

/* Token functions. */
void token(char* start, char* end) {
	/*char* t = NULL;*/
	char t[256];
	khint_t bucket;
	TokenDesc* desc;
	int result;
	int size = end - start;
	
	/*t = malloc(size + 1);*/
	memcpy(t, start, size);
	t[size] = '\0';
	toLower(t);
	
	/* Check whether we already visited this one or not. */
	bucket = kh_get(Tokens, tokens, t);
	if (bucket == kh_end(tokens)) {
		desc			= malloc(sizeof(TokenDesc));
		desc->id		= ++amountTokens;
		desc->occurence = 0;
		desc->token		= malloc(size + 1);
		strcpy(desc->token, t);
		bucket = kh_put(Tokens, tokens, desc->token, &result);
		kh_value(tokens, bucket) = desc;
	}
	else {
		desc = kh_value(tokens, bucket);
	}
	
	++(desc->occurence);
}

void tokenize(char* buffer, char* end) {
	char* start = buffer;
	long size;
	unsigned char c;
	
	for (c = (unsigned char) *buffer; buffer < end; c = ((unsigned char) *++buffer)) {
		/* Delimiters are any spaces, tabs, control characters, etc.
		Specifically: 0 <= c <= 64 && 91 <= c <= 96 && 123 <= c <= 126
		We assume any UTF-8 encoded character with code point > 128 is NOT a delimiter.
		For an English wikipedia dump, this is most likely true.
		It also ignores any single ASCII characters that are floating around.
		*/
		if (c <= 64 || (c >= 91 && c <= 94 ) || c == 96 || (c >= 123 && c <= 126)) {
			size = buffer - start;
			if (size > 1 && size < 16) {
				token(start, buffer);
			}
			
			start = buffer + 1;
		}
	}
}

void processCompletePage(Page* page) {
	char* buffer;
	char* end;
	int size;
	
	if (page->title == NULL || page->content == NULL) {
		/* Screw good practice */
		goto free;
	}
	
	++documentID;
	if (documentID % 10000 == 0) {
		printf( "Processing document id: %lu\n", documentID);
	}
	
	/* Perform an in-place replace/deletion of markup. */
	size		= strlen((char*) page->content);
	buffer		= malloc(size);
	end			= buffer + size;
	
	memcpy(buffer, page->content, size);
	
	/* Remove any SGML comments, template notations ({{ and }}), [[ and ]]s, [ and ]s and
	HTML tags. The first two are stripped altogether, the latter three are stripped but some
	information may be retained, such as description. */
	end = removeComments(buffer, end);
	end = removeTemplate(buffer, end);
	end = removeTags(buffer, end);
	end = removeHyperlinks(buffer, end);
	end = removeHTMLTags(buffer, end);
	
	/* There may still be any []s, but they get ignored. The leftovers are due
	to borked markup. Tokenize the page. */
	tokenize(buffer, end);
	
	free(buffer);
free:
	if (page->title != NULL) {
		xmlFree((xmlChar*) page->title);
	}
	
	if (page->content != NULL) {
		xmlFree((xmlChar*) page->content);
	}

}

void processPage(xmlTextReader* reader) {
	const xmlChar* nodeName;
	int type;
	char ignorePage = 0;
	Page page;
	
	memset(&page, 0, sizeof(Page));
	
	while (xmlTextReaderRead(reader) == 1) {
		type		= xmlTextReaderNodeType(reader);
		nodeName	= xmlTextReaderConstName(reader);
		
		/* Start of an element? */
		if (type == 1) {
			if (xmlStrcmp(nodeName, (const xmlChar*) "title") == 0) {
				processTitle(reader, &page);
			}
			else if (xmlStrcmp(nodeName, (const xmlChar*) "redirect") == 0) {
				/* Ignore this page because a redirect was found. */
				ignorePage = 1;
			}
			else if (!ignorePage && xmlStrcmp(nodeName, (const xmlChar*) "revision") == 0) {
				processRevision(reader, &page);
			}
		}
		/* End element? */
		else if (type == 15) {
			if (xmlStrcmp(nodeName, (const xmlChar*) "page") == 0) {
				processCompletePage(&page);
				return;
			}
		}
	}
}

void processDocument(xmlTextReader* reader) {
	const xmlChar* nodeName;
	
	while (xmlTextReaderRead(reader) == 1) {
		/* Start of an element? */
		if (xmlTextReaderNodeType(reader) == 1) {
			nodeName = xmlTextReaderConstName(reader);
			if (nodeName != NULL && xmlStrcmp (nodeName, (const xmlChar*) "page") == 0) {
				processPage(reader);
			}
		}
	}
}

int inputCallback(void* context, char* buffer, int len) {
	int bzError;
	int bytesRead;
	
	bytesRead = BZ2_bzRead(&bzError, (BZFILE*) context, buffer, len);
	
	if (bzError != BZ_OK && bzError != BZ_STREAM_END) {
		return -1;
	}
	
	totalBytesRead += bytesRead;
	return bytesRead;
}

int closeCallback(void* context) {
	/* Ignore this callback since the bzip2 file is closed in main() */
	return 0;
}

int main(int argc, char** argv) {
	FILE* test;
	xmlTextReader* reader;
	int bzError;
	BZFILE* compressed;
	int bucket;
	
	tokens = kh_init(Tokens);
	if (tokens == NULL) {
		perror("Cannot instantiate map.\n");
		return -1;
	}
	
	test = fopen("chunk-0200.xml.bz2", "r");
	if (test == NULL) {
		perror( "Cannot open file.\n");
		return -1;
	}
	
	compressed = BZ2_bzReadOpen(&bzError, test, 0, 0, NULL, 0);
	if (compressed == NULL) {
		perror("Cannot initialize BZIP2\n");
		return -1;
	}
	
	reader = xmlReaderForIO(inputCallback, closeCallback, compressed, NULL, NULL, 0);
	if (reader == NULL) {
		perror("Cannot initialize LibXML");
		return -1;
	}
	
	processDocument(reader);
	
	printf( "Total uncompressed bytes read: %lu, processed documents: %lu, processed tokens: %lu\n", totalBytesRead, documentID, amountTokens);
	
	for (bucket = kh_begin(tokens); bucket != kh_end(tokens); ++bucket) {
		if (kh_exist(tokens, bucket)) {
			TokenDesc* desc = kh_value(tokens, bucket);
			printf("Token: %s, ID: %lu, occurence: %lu\n", desc->token, desc->id, desc->occurence);
		}
	}
	
	kh_destroy(Tokens, tokens);
	
	/* No cleanup because we are lazy. */
	return 0;
}
