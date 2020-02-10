// See LICENSE.woarc file for license information
#include <stdint.h>
/*
-2: error.
-1: some data may've been written, but there's no more after this.
	you must look for this return code. getting more data after a -1 return is undefined.
 0: happy end. data was written
*/
typedef signed char(*getDataFunc)(void*,char*,size_t,size_t*);
struct fileMeta{
	uint64_t len; // already stored in little endian
	uint64_t lastModified; // already stored in little endian
	// calculated
	uint32_t crc32; // already stored in little endian
	uint64_t posInFile; // already stored in little endian
};
struct userCallbacks{
	void* userData;
	// for the file at the supplied index:
	// fill the len and lastModified fields of the fileMeta struct
	// put the source  custom pointer into the void**
	// you may modify the callbacks if you wish
	signed char(*initSourceFunc)(size_t,struct fileMeta*,void**,struct userCallbacks*,void*);
	// close the supplied source. index is only there if you need it.
	// this source will never be opened again
	signed char(*closeSourceFunc)(size_t,void*,void*); // index, sourceData, userdata
	signed char(*getFilenameFunc)(size_t,char**,void*); // index, desd, userdata. you are in charge of the memory
	signed char(*getCommentFunc)(size_t,char**,void*); // index, dest, userdata. you are in charge of the memory
	getDataFunc getSourceData;
	signed char(*getPropFunc)(size_t,uint8_t*,uint16_t*,void*); // index, property dest, propertyProperty dest, userdata.
};
struct compressState;
struct compressState* allocCompressState(); // just uses malloc to make the state
signed char initCompressState(struct compressState* state, size_t _numSources); // use after allocState. can be used again to delete old state and make a new one with a different size but with same callbacks.
struct userCallbacks* getCallbacks(struct compressState* s); // use after initState. set your things.
signed char makeMoreArchive(struct compressState* state, char* dest, size_t bytesRequested, size_t* numBytesWritten);
//
void setTime(struct compressState* s, time_t _newTime);
