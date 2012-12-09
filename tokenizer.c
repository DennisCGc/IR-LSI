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

#define MM_HEADER "%%MatrixMarket matrix coordinate real general\n"

typedef struct {
	unsigned long id;
	char* token;
	/* Document occurence. */
	unsigned long occurence;
} TokenDesc;

typedef struct {
	unsigned long id;
	unsigned long occurence;
} TokDocDesc;

KHASH_MAP_INIT_STR(Tokens, TokenDesc*)
KHASH_MAP_INIT_INT64(TokDoc, TokDocDesc*)

khash_t(Tokens)* tokens;

long totalBytesRead = 0;
long amountTokens = 0;
long documentID = 0;

typedef struct {
	xmlChar* title;
	xmlChar* content;
} Page;

/* XML parsing functions */
void processTitle(xmlTextReader* reader, Page* page) {
	xmlChar* nodeName;
	int type;
	while (xmlTextReaderRead(reader) == 1) {
		type		= xmlTextReaderNodeType(reader);
		
		/* Start of text? */
		if (type == 3) {
			page->title = xmlTextReaderValue(reader);
		}
		/* End element? */
		else if (type == 15) {
			nodeName = xmlTextReaderName(reader);
			if (xmlStrcmp(nodeName, (const xmlChar*) "title") == 0) {
				xmlFree(nodeName);
				return;
			}
			xmlFree(nodeName);
		}
	}	
}

void processText(xmlTextReader* reader, Page* page) {
	xmlChar* nodeName;
	int type;
	while (xmlTextReaderRead(reader) == 1) {
		type		= xmlTextReaderNodeType(reader);
		
		/* Start of text? */
		if (type == 3) {
			page->content = xmlTextReaderValue(reader);
		}
		/* End element? */
		else if (type == 15) {
			nodeName = xmlTextReaderName(reader);
			if (xmlStrcmp(nodeName, (const xmlChar*) "text") == 0) {
				xmlFree(nodeName);
				return;
			}
			
			xmlFree(nodeName);
		}
	}	
}

void processRevision(xmlTextReader* reader, Page* page) {
	xmlChar* nodeName;
	int type;
	while (xmlTextReaderRead(reader) == 1) {
		type		= xmlTextReaderNodeType(reader);
		nodeName	= xmlTextReaderName(reader);
		
		/* Start of an element? */
		if (type == 1) {
			if (xmlStrcmp(nodeName, (const xmlChar*) "text") == 0) {
				processText(reader, page);
			}
		}
		/* End element? */
		else if (type == 15) {
			if (xmlStrcmp(nodeName, (const xmlChar*) "revision") == 0) {
				xmlFree(nodeName);
				return;
			}
		}
		
		xmlFree(nodeName);
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
void token(char* start, char* end, khash_t(TokDoc)* tokensPerDocument) {
	char t[256];
	khiter_t bucket;
	TokenDesc* desc;
	int result;
	int size = end - start;
	TokDocDesc* tokDocDesc;
	
	/*t = malloc(size + 1);*/
	memcpy(t, start, size);
	t[size] = '\0';
	toLower(t);
	
	/* Check whether we already visited this one or not in the total corpus. */
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
	
	/* Generate statistics for BOW model */
	bucket = kh_get(TokDoc, tokensPerDocument, desc->id);
	if (bucket == kh_end(tokensPerDocument)) {
		bucket		= kh_put(TokDoc, tokensPerDocument, desc->id, &result);
		tokDocDesc	= malloc(sizeof(TokDocDesc));
		tokDocDesc->id = desc->id;
		tokDocDesc->occurence = 0;
		kh_value(tokensPerDocument, bucket) = tokDocDesc;
		
		/* Update document frequency. */
		++(desc->occurence);
	}
	else {
		tokDocDesc = kh_value(tokensPerDocument, bucket);
	}
	
	++tokDocDesc->occurence;
	
}

void tokenize(char* buffer, char* end, khash_t(TokDoc)* tokensPerDocument) {
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
				token(start, buffer, tokensPerDocument);
			}
			
			start = buffer + 1;
		}
	}
}

int compare(const void* a, const void* b) {
	const TokDocDesc* e1 = (TokDocDesc*) a;
	const TokDocDesc* e2 = (TokDocDesc*) b;
	if (e1->id > e2->id) return 1;
	else if (e1->id < e2->id) return -1;
	else return 0;
}

void processCompletePage(Page* page, FILE* docBow, FILE* docID) {
	char* buffer;
	char* end;
	int size;
	khash_t(TokDoc)* tokensPerDocument;
	khiter_t bucket;
	TokDocDesc* tokDocDescs;
	TokDocDesc* desc;
	int i;
	int mapSize;
	int result;
	
	if (page->title == NULL || page->content == NULL) {
		/* Screw good practice */
		goto free;
	}
	
	tokensPerDocument = kh_init(TokDoc);
	
	++documentID;
	if (documentID % 1000 == 0) {
		printf( "Processing document id: %lu, amount unique tokens: %lu, amount bytes processed: %lu\n", documentID, amountTokens, totalBytesRead);
	}
	
	fprintf(docID, "%lu\t%s\n", documentID, page->title);
	
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
	tokenize(buffer, end, tokensPerDocument);
	
	/* Write frequencies to doc. Make sure it is sorted. */
	mapSize = kh_size(tokensPerDocument);
	tokDocDescs = malloc(sizeof(TokDocDesc) * mapSize);
	for (i = 0, bucket = kh_begin(tokensPerDocument); bucket != kh_end(tokensPerDocument); ++bucket) {
		if (kh_exist(tokensPerDocument, bucket)) {
			desc = kh_value(tokensPerDocument, bucket);
			memcpy(&tokDocDescs[i], desc, sizeof(TokDocDesc));
			kh_del(TokDoc, tokensPerDocument, bucket);
			free(desc);
			++i;
		}
	}
	
	result = heapsort(tokDocDescs, mapSize, sizeof(TokDocDesc), compare);
	for (i = 0; i < mapSize; ++i) {
		desc = &tokDocDescs[i];
		fprintf(docBow, "%lu\t%lu\t%lu\n", documentID, desc->id, desc->occurence);
	}
	
	free(tokDocDescs);
	free(buffer);
	kh_destroy(TokDoc, tokensPerDocument);
	
free:
	if (page->title != NULL) {
		xmlFree((xmlChar*) page->title);
	}
	
	if (page->content != NULL) {
		xmlFree((xmlChar*) page->content);
	}

}

void processPage(xmlTextReader* reader, FILE* docBow, FILE* docID) {
	xmlChar* nodeName;
	int type;
	char ignorePage = 0;
	Page page;
	
	memset(&page, 0, sizeof(Page));
	
	while (xmlTextReaderRead(reader) == 1) {
		type		= xmlTextReaderNodeType(reader);
		nodeName	= xmlTextReaderName(reader);
		
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
				processCompletePage(&page, docBow, docID);
				xmlFree(nodeName);
				return;
			}
		}
		
		xmlFree(nodeName);
	}
}

void processDocument(xmlTextReader* reader, FILE* docBow, FILE* docID) {
	xmlChar* nodeName;
	
	while (xmlTextReaderRead(reader) == 1) {
		/* Start of an element? */
		if (xmlTextReaderNodeType(reader) == 1) {
			
			nodeName = xmlTextReaderName(reader);
			if (nodeName != NULL && xmlStrcmp (nodeName, (const xmlChar*) "page") == 0) {
				processPage(reader, docBow, docID);
			}
			
			xmlFree(nodeName);
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

int help() {
	printf("Syntax: tokenizer [input] [bow output] [word ID output] [docID output]\n");
	return 0;
}

int main(int argc, char** argv) {
	FILE* wiki;
	FILE* docBow;
	FILE* wordID;
	FILE* docID;
	
	xmlTextReader* reader;
	int bzError;
	BZFILE* compressed;
	khiter_t bucket;
	
	/* We need input filename, output name for BOW per document and output filename for word IDs in total. */
	if (argc != 5) {
		return help();
	}
	
	tokens = kh_init(Tokens);
	if (tokens == NULL) {
		perror("Cannot instantiate map.\n");
		return -1;
	}
	
	wiki = fopen(argv[1], "r");
	if (wiki == NULL) {
		perror("Cannot open input file.\n");
		return -1;
	}
	
	docBow = fopen(argv[2], "w");
	if (docBow == NULL) {
		perror("Cannot create output file for BOW\n");
		return -1;
	}
	
	fwrite(MM_HEADER, sizeof(MM_HEADER), 1, docBow);
	
	wordID = fopen(argv[3], "w");
	if (wordID == NULL) {
		perror("Cannot create output file for word IDs\n");
		return -1;
	}
	
	docID = fopen(argv[4], "w");
	if (wordID == NULL) {
		perror("Cannot create output file for doc IDs\n");
		return -1;
	}
	
	compressed = BZ2_bzReadOpen(&bzError, wiki, 0, 0, NULL, 0);
	if (compressed == NULL) {
		perror("Cannot initialize BZIP2\n");
		return -1;
	}
	
	reader = xmlReaderForIO(inputCallback, closeCallback, compressed, NULL, NULL, 0);
	if (reader == NULL) {
		perror("Cannot initialize LibXML");
		return -1;
	}
	
	processDocument(reader, docBow, docID);
	fclose(docID);
	fclose(docBow);
	
	printf( "Total uncompressed bytes read: %lu, processed documents: %lu, processed tokens: %lu\n", totalBytesRead, documentID, amountTokens);
	
	printf("Writing word IDs\n");
	for (bucket = kh_begin(tokens); bucket != kh_end(tokens); ++bucket) {
		if (kh_exist(tokens, bucket)) {
			TokenDesc* desc = kh_value(tokens, bucket);
			fprintf(wordID, "%lu\t%s\t%lu\n", desc->id, desc->token, desc->occurence);
			
			kh_del(Tokens, tokens, bucket);
			free(desc->token);
			free(desc);
		}
	}
	
	kh_destroy(Tokens, tokens);
	
	fclose(wordID);
	
	/* No further cleanup because we are lazy. */
	return 0;
}
