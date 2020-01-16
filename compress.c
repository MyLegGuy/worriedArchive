// todo - maybe just make a state "go to in nextState"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/stat.h> // required for FILE implementation

/*
-2: error.
-1: some data may've been written, but there's no more after this.
	you must look for this return code. getting more data after a -1 return is undefined.
 0: happy end. data was written
*/
typedef signed char(*getDataFunc)(void*,char*,size_t,size_t*);

enum writeState{
	WRITESTATE_UNKNOWN=0,
	WRITESTATE_DONE,
	WRITESTATE_PUTBUFF, // write whatever is in the buffer and then proceed to nextState
	WRITESTATE_BITBYBIT, // using the current getBitFunc, write all the data. proceeds to nextState
	//
	WRITESTATE_GOTONEXTFILE, // increment curSourceIndex and be done or goto WRITESTATE_MFHMAGIC
	WRITESTATE_TOPMAGIC, // put top magic into buffer then WRITESTATE_PUTBUFF
	WRITESTATE_FILEHEADER, // put file header into buffer then WRITESTATE_PUTBUFF
	WRITESTATE_FILEDATA, // put file data in getBitFunc then WRITESTATE_BITBYBIT
	WRITESTATE_FILENAME, // get filename then WRITESTATE_PUTBUFF
	WRITESTATE_COMMENT, // get comment then WRITESTATE_PUTBUFF
	WRITESTATE_MFHMAGIC, // put "middle file header magic" then  WRITESTATE_PUTBUFF then WRITESTATE_FILEHEADER
	WRITESTATE_TABLESTART, // TODO
};

// must be at least big enough to hold the biggest header without the variable sized strings
#define COMPRESSSTATEBUFFSIZE 256
struct fileMeta{
	uint64_t len;
	uint64_t lastModified;
	uint32_t crc32;
};
struct compressState{
	size_t numSources;
	size_t curSourceIndex;
	struct fileMeta* cachedMeta;
	signed char wstate;
	// state-specific variabkes
	char isBottomTable; 
	void* curSourceData;
	getDataFunc getBitFunc; // for WRITESTATE_BITBYBIT
	void* getBitFuncData; // for WRITESTATE_BITBYBIT
	signed char nextState; // some states go to this state next
	uint64_t localProgress; // in WRITESTATE_PUTBUFF represents next char to be written
	size_t usedBuff; // how much of the putBuff buffer is used
	char* putBuff; // pointer to what to write for WRITESTATE_PUTBUFF. usually points to _internalBuff
	char _internalBuff[COMPRESSSTATEBUFFSIZE]; // internal buffer for WRITESTATE_PUTBUFF.
	// user functions
	void* userData;
	// for the file at the supplied index:
	// fill the len and lastModified fields of the fileMeta struct
	// put the source  custom pointer into the void**
	signed char(*initSourceFunc)(size_t,struct fileMeta*,void**,void*);
	// close the supplied source. index is only there if you need it.
	// this source will never be opened again
	signed char(*closeSourceFunc)(size_t,void*,void*); // index, sourceData, userdata
	signed char(*getFilenameFunc)(size_t,char**,void*); // index, desd, userdata. you are in charge of the memory
	signed char(*getCommentFunc)(size_t,char**,void*); // index, dest, userdata. you are in charge of the memory
	getDataFunc getDataFunc;
};

#define MFHMAGIC "WARCFILE"
#define TOPMAGIC "WARCARCHIVEMAGIC"
#define WRITEVERSION 1

// write until src is done or n bytes are written. returns number of bytes written.
size_t strncpycnt(char* dest, const char* src, size_t n){
	if (!n){
		return 0;
	}
	size_t written=0;
	for(;*src!='\0';){
		*(dest++)=*(src++);
		if (++written==n){
			break;
		}
	}
	return written;
}
signed char lowGetStrBit(struct compressState* state, char* dest, size_t bytesRequested, size_t* numBytesWritten, const char* srcStr){
	const char* _startSrc = srcStr+state->localProgress;
	size_t written =strncpycnt(dest,_startSrc,bytesRequested);
	state->localProgress+=written;
	*numBytesWritten=written;
	return (written==bytesRequested) ? 0 : -1;
}
// void* data, char* dest, size_t bytesRequested, size_t* numBytesWritten
/* signed char getFilenameBit(void* _uncastState, char* dest, size_t bytesRequested, size_t* numBytesWritten){ */
/* 	struct compressState* state = _uncastState; */
/* 	return lowGetStrBit(state,dest,bytesRequested,numBytesWritten,state->filenames[state->curSourceIndex]); */
/* } */
void resetBuffState(struct compressState* state, signed char _nextState){
	state->wstate=WRITESTATE_PUTBUFF;
	state->nextState=_nextState;
	state->localProgress=0;
	state->putBuff=state->_internalBuff;
}
// returns 0 if worked, else on error
signed char initState(struct compressState* state, signed char _newState){
top:
	state->localProgress=0;
	switch(_newState){
		case WRITESTATE_TOPMAGIC:
			resetBuffState(state,WRITESTATE_MFHMAGIC);
			memcpy(state->putBuff,TOPMAGIC,strlen(TOPMAGIC));
			(state->putBuff)[strlen(TOPMAGIC)]=WRITEVERSION;
			state->usedBuff=strlen(TOPMAGIC)+1;
			break;
		case WRITESTATE_MFHMAGIC:
			resetBuffState(state,WRITESTATE_FILEHEADER);
			strcpy(state->putBuff,MFHMAGIC);
			state->usedBuff=strlen(state->putBuff);
			break;
		case WRITESTATE_FILEHEADER:
			// prepare file buffer here
			if (!state->isBottomTable){
				if (state->initSourceFunc(state->curSourceIndex,state->cachedMeta+state->curSourceIndex,&state->curSourceData,state->userData)){
					return -2;
				}
			}
			resetBuffState(state,WRITESTATE_FILENAME);
			strcpy(state->putBuff,"HEADERGOESHERE");
			state->usedBuff=strlen(state->putBuff);
			break;
		case WRITESTATE_FILENAME:
			resetBuffState(state,WRITESTATE_COMMENT);
			if (state->getFilenameFunc(state->curSourceIndex,&(state->putBuff),state->userData)){
				return -2;
			}
			state->usedBuff=strlen(state->putBuff);
			break;
		case WRITESTATE_COMMENT:
			resetBuffState(state,WRITESTATE_FILEDATA);
			if (state->getCommentFunc(state->curSourceIndex,&(state->putBuff),state->userData)){
				return -2;
			}
			state->usedBuff=strlen(state->putBuff);
			break;
		case WRITESTATE_FILEDATA:
			if (!state->isBottomTable){
				state->wstate=WRITESTATE_BITBYBIT;
				state->getBitFunc=state->getDataFunc;
				state->getBitFuncData=state->curSourceData;
				state->nextState=WRITESTATE_GOTONEXTFILE;
			}else{
				// skip file data in bottom metadata table
				_newState=WRITESTATE_GOTONEXTFILE;
				goto top;
			}
			break;
		case WRITESTATE_GOTONEXTFILE:
			if (!state->isBottomTable){
				if (state->closeSourceFunc(state->curSourceIndex,state->curSourceData,state->userData)){
					return -2;
				}
			}
			if (++state->curSourceIndex==state->numSources){ // if we're at the end
				if (!state->isBottomTable){
					// TEMP - TEMP - TEMP - normally, proceed to footer table. in this case, just be done.
					state->wstate=WRITESTATE_DONE;
					//state->isBottomTable=1;
				}else{
					state->wstate=WRITESTATE_DONE;
				}
			}else{
				// proceed to next file as normal
				_newState=WRITESTATE_MFHMAGIC;
				goto top;
			}
			break;
		default:
			printf("invalid next state %d\n",state->nextState);
			return -1;
	}
	return 0;
}
/*
-2: error.
-1: some data was written. there is no more though.
 0: happy end. 	numBytesWritten is updated.
*/
signed char makeMoreArchive(struct compressState* state, char* dest, size_t bytesRequested, size_t* numBytesWritten){
	*numBytesWritten=0;
top:
	{
		printf("at state %d->%d\n",state->wstate,state->nextState);
		char gotoNextState=0;
		size_t justWrote; // number of bytes you wrote in this iteration
		switch(state->wstate){
			case WRITESTATE_PUTBUFF:
			{
				char* curSource = state->putBuff+state->localProgress;
				// if we have enough left to fill the dest
				if (state->localProgress+bytesRequested<=state->usedBuff){
					memcpy(dest,curSource,bytesRequested);
					state->localProgress+=bytesRequested;
					*numBytesWritten+=bytesRequested;
				}else{
					// write what's left
					justWrote = state->usedBuff-state->localProgress;
					memcpy(dest,curSource,justWrote);
					// continue on
					*numBytesWritten+=justWrote;
					gotoNextState=1;
				}
			}
			break;
			case WRITESTATE_FILENAME:
				break;
			case WRITESTATE_COMMENT:
				break;
			case WRITESTATE_FILEDATA:
				break;
			case WRITESTATE_DONE:
				return -1;
				break;
			case WRITESTATE_BITBYBIT:
			{
				signed char readRet = state->getBitFunc(state->getBitFuncData,dest,bytesRequested,&justWrote);
				*numBytesWritten+=justWrote;
				if (readRet==-1){
					gotoNextState=1;
				}else{
					return readRet;
				}
			}
			break;
			default:
				puts("unimplemented state");
				return -1;
		}
		if (gotoNextState){
			bytesRequested-=justWrote;
			dest+=justWrote;
			if (initState(state,state->nextState)){
				return -2;
			}
			goto top;
		}
	}
	return 0;
}

void prepareState(struct compressState* state, size_t _numSources){
	state->numSources=_numSources;
	state->cachedMeta=malloc(sizeof(struct fileMeta)*_numSources);
	// queue magic as first thing to do by using buffer write state that instantly ends
	resetBuffState(state,WRITESTATE_TOPMAGIC);
	state->usedBuff=0; // will cause to go to WRITESTATE_TOPMAGIC instantly
	state->curSourceIndex=0;
	state->isBottomTable=0;
}
signed char getFileMetadata(void* src, size_t* ret){
	struct stat st;
	if (fstat(fileno(src),&st)){
		return 1;
	}else{
		*ret=st.st_size;
		return 0;
	}
}

signed char myInitSource(size_t i, struct fileMeta* infoDest, void** srcDest, void* _userData){
	infoDest->len=0;
	infoDest->lastModified=0;
	*srcDest=fopen(((char**)_userData)[i],"rb");
	return *srcDest ? 0 : -2;
}
signed char myCloseSource(size_t i, void* _closeThis, void* _userData){
	return fclose(_closeThis)==0 ? 0 : -2;
}
signed char myGetFilename(size_t i, char** dest, void* _userData){
	*dest=((char**)_userData)[i];
	return 0;
}
signed char myGetComment(size_t i, char** dest, void* _userData){
	*dest="";
	return 0;
}
signed char mygetDataFunc(void* src, char* dest, size_t requested, size_t* actual){
	*actual = fread(dest,1,requested,src);
	if (*actual!=requested){
		if (ferror(src)){
			return -2;
		}else if (feof(src)){
			return -1;
		}
	}
	return 0;
}

#define TESTCHUNKSIZE 1
int main(int argc, char** args){
	char* testFiles[3] = {
		"/tmp/a",
		"/tmp/b",
		"/tmp/c",
	};
	struct compressState s;
	s.userData=testFiles;
	s.initSourceFunc=myInitSource;
	s.closeSourceFunc=myCloseSource;
	s.getFilenameFunc=myGetFilename;
	s.getCommentFunc=myGetComment;
	s.getDataFunc=mygetDataFunc;
	prepareState(&s,sizeof(testFiles)/sizeof(char*));

	// write archive to file
	FILE* dest = fopen("/tmp/outarc","wb");
	char writeBuff[TESTCHUNKSIZE];
	size_t gotBytes;
	signed char lastRet;
	while((lastRet=makeMoreArchive(&s,writeBuff,TESTCHUNKSIZE,&gotBytes))!=-2){
		if (fwrite(writeBuff,1,gotBytes,dest)!=gotBytes){
			lastRet=-2;
			break;
		}
		if (lastRet==-1){
			break;
		}
	}
	if (lastRet==-2){
		printf("error!\n");
	}
	fclose(dest);
}
