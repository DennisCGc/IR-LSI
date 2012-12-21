/*
Simple tokenizer for wikipedia dump
- Uses bzip2 library for decompression on the fly
- Uses the expat xml reader to parse the document.
- Uses klib/khash for hash maps.
- Uses own simple buffer implementation to implement strings.
- The rest is just "hacked" up together in order to make it work :-)

Compiling on FreeBSD:
gcc -O2 -Wall -pedantic --std=c99 -o tokenizer tokenizer.c buffer.c -lbz2 \
	-lexpat -L/usr/local/lib/ -I/usr/local/include

*/

#include <bzlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <expat.h>
#include "khash.h"
#include "buffer.h"

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

#define STATE_IGNORE 0
#define STATE_IN_TITLE 1
#define STATE_IN_TEXT 2
#define STATE_IN_PAGE 3

struct ParsingState {
	char state;
	Buffer* title;
	Buffer* text;
	
	FILE* docBow;
	FILE* docID;
};

KHASH_MAP_INIT_STR(Tokens, TokenDesc*)
KHASH_MAP_INIT_INT64(TokDoc, TokDocDesc*)

khash_t(Tokens)* tokens;

long totalBytesRead = 0;
long amountTokens = 0;
long documentID = 0;
long amountLines = 0;

/* Cleanup any left overs in order to make sure the title does not copied over to a new page. */
static inline void resetState(struct ParsingState* state) {
	bufferReset(state->title);
	bufferReset(state->text);
	state->state = STATE_IGNORE;
}

static inline void token(const char* begin, const char* end,  khash_t(TokDoc)* tokensPerDocument) {
	unsigned int size = end - begin;
	int i;
	char* p;
	char temp[49];
	khiter_t bucket;
	TokenDesc* desc;
	TokDocDesc* tokDocDesc;
	int result;
	
	if (size < 2 || size > 48) {
		return;
	}
	
	memcpy(temp, begin, size);
	temp[size] = 0;
	
	for (i = 0, p = temp; i < size; ++p, ++i) {
		*p = tolower(*p);
	}
	
	/* Make sure our word is registered in our global word list. */
	bucket = kh_get(Tokens, tokens, temp);
	if (bucket == kh_end(tokens)) {
		desc			= malloc(sizeof(TokenDesc));
		desc->id		= ++amountTokens;
		desc->occurence = 0;
		desc->token		= malloc(size + 1);
		strcpy(desc->token, temp);
		bucket = kh_put(Tokens, tokens, desc->token, &result);
		kh_value(tokens, bucket) = desc;
	}
	else {
		desc = kh_value(tokens, bucket);
	}
	
	/* Add the word, if necessary. to the per document word list. */ 
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

/* Skips, recursively, any template regardless of content. */
const char* removeTemplate(const char* page, const char* pageEnd) {
	char c;
	char previous = 0;
	
	while (page < pageEnd) {
		c = *page++;
		
		if (previous == '{' && c == '{') {
			page = removeTemplate(page + 1, pageEnd);
		}
		else if (previous == '}' && c == '}') {
			return page + 1;
		}
		
		previous = *(page - 1);
	}
	
	return pageEnd;
}

/* Ignores anything between < .. >, even SGML-style comments. */
const char* removeHTMLTags(const char* page, const char* pageEnd) {
	char c;
	unsigned int openings = 1;
	
	while (page < pageEnd) {
		c = *page++;
		
		if (c == '<') {
			++openings;
		}
		else if (c == '>') {
			--openings;
			if (openings == 0) {
				break;
			}
		}
	}
	
	return page;
}

/*
Technically, this function 'cheats'. It ignores everything until either a space or ] is encountered.
Any subsequent ] is ignored either way.
*/
const char* removeAutoLinks(const char* page, const char* pageEnd) {
	char c;
	
	for (c = *page; page < pageEnd; c = *(++page)) {
		if (c == ' ' || c == ']') {
			++page;
			break;
		}
	}
	
	return page;
}

/*
This function also cheats. It keeps track to where things should be ignored and any left over ]] is ignored by the main processing
function.
*/
const char* removeTags(const char* page, const char* pageEnd) {
	const char* begin = page;
	char shouldIgnore = 0;
	char metaText = 0;
	char previous = 0;
	char c;
	
	while (page < pageEnd) {
		c = *page++;
		
		if (c == ']' && previous == ']') {
			if (shouldIgnore) {
				return page;
			}
			
			return begin;
		}
		else if (c == '|') {
			begin			= page;
			shouldIgnore	= 0;
			metaText		= 1;
		}
		else if (!metaText && c == ':') {
			shouldIgnore	= 1;
		}
		
		previous = c;
	}
	
	return pageEnd;
}

int compare(const void* a, const void* b) {
	const TokDocDesc* e1 = (TokDocDesc*) a;
	const TokDocDesc* e2 = (TokDocDesc*) b;
	if (e1->id > e2->id) return 1;
	else if (e1->id < e2->id) return -1;
	else return 0;
}

void writeFrequencies(struct ParsingState* parseState, khash_t(TokDoc)* tokensPerDocument) {
	khint_t mapSize;
	TokDocDesc* tokDocDescs;
	TokDocDesc* desc;
	khiter_t bucket;
	unsigned int i;
	int result;

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
		fprintf(parseState->docBow, "%lu %lu %lu\n", documentID, desc->id, desc->occurence);
	}
	
	free(tokDocDescs);
}

void processDocument(struct ParsingState* parseState) {
	const char* page;
	const char* pageEnd;
	const char* beginWord;
	char c;
	char previous = 0;
	khash_t(TokDoc)* tokensPerDocument;
	
	if (parseState->title->currentsize == 0 || parseState->text->currentsize == 0) {
		return;
	}
	
	tokensPerDocument	= kh_init(TokDoc);
	page				= parseState->text->buffer;
	pageEnd				= page + parseState->text->currentsize;
	beginWord			= page;
	
	++documentID;
	if (documentID % 1000 == 0) {
		printf( "Processing document id: %lu, amount unique tokens: %lu, amount bytes processed: %lu\n", documentID, amountTokens, totalBytesRead);
	}
	
	fprintf(parseState->docID, "%lu\t%.*s\n", documentID, parseState->title->currentsize, parseState->title->buffer);
	
	while (page < pageEnd) {
		c = *page;
		
		/*
		First check this character is a possible delimiter.
		Delimiters are any spaces, tabs, control characters, etc.
		Specifically: 0 <= c <= 64 && 91 <= c <= 96 && 123 <= c <= 126
		We assume any UTF-8 encoded character with code point > 128 is NOT a delimiter.
		For an English wikipedia dump, this is most likely true.
		It also ignores any single ASCII characters that are floating around.
		*/
		if (!((c >= 0 && c <= 64) || (c >= 91 && c <= 94 ) || c == 96 || (c >= 123 && c <= 126))) {
			++page;
			previous = c;
			continue;
		}
		
		token(beginWord, page, tokensPerDocument);
		++page;
		
		if (c == '{' && previous == '{') {
			page = removeTemplate(page, pageEnd);
			previous = 0;
		}
		else if (c == '<') {
			page = removeHTMLTags(page, pageEnd);
			previous = 0;
		}
		else if (previous == '[') {
			if (c == '[') {
				page = removeTags(page, pageEnd);
			}
			else {
				page = removeAutoLinks(page, pageEnd);
			}
			previous = 0;
		}
		else {
			previous = c;
		}
		
		beginWord = page;
	}
	
	/* Write frequencies already clears tokensPerDocument. */
	amountLines += kh_size(tokensPerDocument);
	writeFrequencies(parseState, tokensPerDocument);
	kh_destroy(TokDoc, tokensPerDocument);
}

void beginElementHandler(void* data, const XML_Char* element, const XML_Char **atts) {
	struct ParsingState *state = (struct ParsingState*) data;
	
	if (state->state != STATE_IGNORE) {
		if (strcmp(element, "title") == 0) {
			bufferReset(state->title);
			state->state = STATE_IN_TITLE;
		}
		else if (strcmp(element, "redirect") == 0) {
			resetState(state);
		}
		else if (strcmp(element, "text") == 0) {
			bufferReset(state->text);
			state->state = STATE_IN_TEXT;
		}
	}
	else if (strcmp(element, "page") == 0) {
		state->state = STATE_IN_PAGE;
	}
}

void endElementHandler(void *data, const char* element) {
	struct ParsingState *state = (struct ParsingState*) data;
	
	if (state->state != STATE_IGNORE) {
		if (strcmp(element, "page") == 0) {
			processDocument(state);
			resetState(state);
		}
		else if (strcmp(element, "title") == 0 || strcmp(element, "text") == 0) {
			state->state = STATE_IN_PAGE;
		}
	}
}

void characterHandler(void* data, const XML_Char* buffer, int length) {
	struct ParsingState* state = (struct ParsingState*) data;
	
	switch (state->state) {
		case STATE_IN_TITLE:
			bufferAdd(state->title, (char*) buffer, length);
			break;
		case STATE_IN_TEXT:
			/* Copy text first before processing it, because the text may be ignored afterwards. */
			bufferAdd(state->text, (char*) buffer, length);
			break;
	}
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
	
	XML_Parser parser;
	int bzError;
	int bytesRead;
	unsigned long i;
	BZFILE* compressed;
	khiter_t bucket;
	char spaces[64];
	char buffer[16384];
	struct ParsingState state;
	
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
	
	memset(spaces, 32, sizeof(spaces));
	spaces[sizeof(spaces) - 1] = '\n';
	fwrite(MM_HEADER, sizeof(MM_HEADER), 1, docBow);
	fwrite(spaces, sizeof(spaces), 1, docBow);
	
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
	
	parser = XML_ParserCreate("UTF-8");
	if (parser == NULL) {
		perror("Cannot initialize xml parser");
		return -1;
	}
	
	XML_SetUserData(parser, &state);
	XML_SetElementHandler(parser, beginElementHandler, endElementHandler);
	XML_SetCharacterDataHandler(parser, characterHandler);
	
	memset(&state, 0, sizeof(state));
	state.title		= bufferInit();
	state.text		= bufferInit();
	state.docBow	= docBow;
	state.docID		= docID;
	
	while (1) {
		bytesRead = BZ2_bzRead(&bzError, compressed, buffer, sizeof(buffer));
		
		if (bzError != BZ_OK && bzError != BZ_STREAM_END) {
			break;
		}
		
		if (bytesRead == 0) {
			break;
		}
		
		totalBytesRead += bytesRead;
		if (!XML_Parse(parser, buffer, bytesRead, bzError == BZ_STREAM_END)) {
			perror("XML parsing error");
			return -1;
		}
	}
	
	/* Cleanup any parsing data */
	XML_ParserFree(parser);
	BZ2_bzReadClose(&bzError, compressed);
	bufferDestroy(state.title);
	bufferDestroy(state.text);
	fclose(wiki);
	
	fclose(docID);
	
	printf("Total uncompressed bytes read: %lu, processed documents: %lu, processed tokens: %lu\n",
		totalBytesRead, documentID, amountTokens);
	
	fseek(docBow, sizeof(MM_HEADER) - 1, SEEK_SET);
	fprintf(docBow, "%lu %lu %lu", documentID, amountTokens, amountLines);
	fclose(docBow);
	
	setbuf(stdout, NULL);
	printf("Writing word IDs: ");
	
	for (i = 0, bucket = kh_begin(tokens); bucket != kh_end(tokens); ++bucket, ++i) {
		if (kh_exist(tokens, bucket)) {
			TokenDesc* desc = kh_value(tokens, bucket);
			fprintf(wordID, "%lu\t%s\t%lu\n", desc->id, desc->token, desc->occurence);
			
			kh_del(Tokens, tokens, bucket);
			free(desc->token);
			free(desc);
			
			if (i % 10000 == 0) {
				putchar('.');
			}
		}
	}
	
	printf("\n");
	kh_destroy(Tokens, tokens);
	
	fclose(wordID);
	
	return 0;
}
