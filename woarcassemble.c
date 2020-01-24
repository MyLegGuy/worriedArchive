#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <endian.h>
#include <zlib.h>
#include <time.h>
#include "woarcassemble.h"

enum writeState{
	WRITESTATE_UNKNOWN=0,
	WRITESTATE_DONE,
	WRITESTATE_PUTBUFF, // write whatever is in the buffer and then proceed to nextState
	WRITESTATE_BITBYBIT, // using the current getBitFunc, write all the data. proceeds to nextState
	//
	WRITESTATE_GOTONEXTFILE, // increment curSourceIndex and be done or goto WRITESTATE_FILEHEADER
	WRITESTATE_TOPMAGIC, // put top magic into buffer then WRITESTATE_PUTBUFF
	WRITESTATE_THISTIMESTAMP, // put the archive creation time from state->cachedTime through WRITESTATE_PUTBUFF
	WRITESTATE_FILEHEADER, // put file header into buffer then WRITESTATE_PUTBUFF
	WRITESTATE_FILEDATA, // put file data in getBitFunc then WRITESTATE_BITBYBIT
	WRITESTATE_FILENAME, // get filename then WRITESTATE_PUTBUFF
	WRITESTATE_FINISHHASH, // After the file data, finish hash up and write it. only if not in bottomt table.
	WRITESTATE_COMMENT, // get comment then WRITESTATE_PUTBUFF
	WRITESTATE_TABLESTART, // write table start magic through WRITESTATE_PUTBUFF
};
// must be at least big enough to hold the biggest header without the variable sized strings
#define COMPRESSSTATEBUFFSIZE 256
struct compressState{
	size_t numSources;
	size_t curSourceIndex;
	struct fileMeta* cachedMeta;
	signed char wstate;
	size_t filePos; // added to every time data is written by makeMoreArchive
	// state-specific variabkes
	char isBottomTable; 
	void* curSourceData;
	getDataFunc getBitFunc; // for WRITESTATE_BITBYBIT
	void* getBitFuncData; // for WRITESTATE_BITBYBIT
	signed char nextState; // some states go to this state next
	uint64_t localProgress; // in WRITESTATE_PUTBUFF represents next char to be written. in WRITESTATE_BITBYBIT represents total written
	size_t usedBuff; // how much of the putBuff buffer is used
	char* putBuff; // pointer to what to write for WRITESTATE_PUTBUFF. usually points to _internalBuff
	char _internalBuff[COMPRESSSTATEBUFFSIZE]; // internal buffer for WRITESTATE_PUTBUFF.
	time_t cachedTime; // to ensure the time written stays the same
	// user functions
	struct userCallbacks user;
};

#include "formatInfo.h"

signed char getFileBitAndHash(void* _uncastState, char* dest, size_t bytesRequested, size_t* numBytesWritten){
	struct compressState* state = _uncastState;
	signed char _getRet = state->user.getSourceData(state->curSourceData,dest,bytesRequested,numBytesWritten);
	if (_getRet==-2){
		return -2;
	}
	if (*numBytesWritten!=0){
		if (*numBytesWritten<=UINT32_MAX){
			state->cachedMeta[state->curSourceIndex].crc32 = crc32(state->cachedMeta[state->curSourceIndex].crc32,(const Bytef*)dest,*numBytesWritten);
		}else{
			// can't pass more than uInt bytes at once
			uint64_t _hashBytesLeft=*numBytesWritten;
			uint64_t _buffOffset=0;
			while(_hashBytesLeft>0){
				uint32_t _passBytes = _hashBytesLeft>=UINT32_MAX ? UINT32_MAX : _hashBytesLeft;
				state->cachedMeta[state->curSourceIndex].crc32 = crc32(state->cachedMeta[state->curSourceIndex].crc32,(const Bytef*)(dest+_buffOffset),_passBytes);
				_hashBytesLeft-=_passBytes;
				_buffOffset+=_hashBytesLeft;
			}
		}
	}
	return _getRet;
}
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
void resetBuffState(struct compressState* state, signed char _nextState){
	state->wstate=WRITESTATE_PUTBUFF;
	state->nextState=_nextState;
	state->localProgress=0;
	state->putBuff=state->_internalBuff;
}
// returns 0 if worked, else on error
signed char initState(struct compressState* state, signed char _newState){
top:
	switch(_newState){
		case WRITESTATE_TOPMAGIC:
			resetBuffState(state,WRITESTATE_THISTIMESTAMP);
			memcpy(state->putBuff,TOPMAGIC,strlen(TOPMAGIC));
			(state->putBuff)[strlen(TOPMAGIC)]=WRITEVERSION;
			state->usedBuff=strlen(TOPMAGIC)+1;
			break;
		case WRITESTATE_THISTIMESTAMP:
			resetBuffState(state,WRITESTATE_FILEHEADER);
			uint64_t _fixedTime = htole64(state->cachedTime);
			memcpy(state->putBuff,&_fixedTime,sizeof(uint64_t));
			state->usedBuff=sizeof(uint64_t);
			break;
		case WRITESTATE_FILEHEADER:
			// prepare file buffer here
			if (!state->isBottomTable){
				if (state->user.initSourceFunc(state->curSourceIndex,state->cachedMeta+state->curSourceIndex,&state->curSourceData,state->user.userData)){
					return -2;
				}
				state->cachedMeta[state->curSourceIndex].len = htole64(state->cachedMeta[state->curSourceIndex].len);
				state->cachedMeta[state->curSourceIndex].lastModified = htole64(state->cachedMeta[state->curSourceIndex].lastModified);
				state->cachedMeta[state->curSourceIndex].posInFile=htole64(state->filePos);
			}
			resetBuffState(state,WRITESTATE_FILENAME);
			// write magic
			strcpy(state->putBuff,state->isBottomTable ? BGFMAGIC : MFHMAGIC);
			state->usedBuff=strlen(state->putBuff);
			// write file length
			memcpy(state->putBuff+state->usedBuff,&(state->cachedMeta[state->curSourceIndex].len),sizeof(uint64_t));
			state->usedBuff+=sizeof(uint64_t);
			// write filename length
			uint16_t _tempLen;
			char* _tempStr;
			if (state->user.getFilenameFunc(state->curSourceIndex,&_tempStr,state->user.userData)){
				return -2;
			}
			_tempLen=htole16(strlen(_tempStr));
			memcpy(state->putBuff+state->usedBuff,&_tempLen,sizeof(uint16_t));
			state->usedBuff+=sizeof(uint16_t);
			// write comment length
			if (state->user.getCommentFunc(state->curSourceIndex,&_tempStr,state->user.userData)){
				return -2;
			}
			_tempLen=htole16(strlen(_tempStr));
			memcpy(state->putBuff+state->usedBuff,&_tempLen,sizeof(uint16_t));
			state->usedBuff+=sizeof(uint16_t);
			// write last modified time
			memcpy(state->putBuff+state->usedBuff,&(state->cachedMeta[state->curSourceIndex].lastModified),sizeof(uint16_t));
			state->usedBuff+=sizeof(uint64_t);
			// write file property & propertyProperty
			uint8_t _writeProperty;
			uint16_t _writePropertyProperty;
			if (state->user.getPropFunc(state->curSourceIndex,&_writeProperty,&_writePropertyProperty,state->user.userData)){
				return -2;
			}
			_writePropertyProperty = htole16(_writePropertyProperty);
			memcpy(state->putBuff+state->usedBuff,&_writeProperty,sizeof(uint8_t));
			state->usedBuff+=sizeof(uint8_t);
			memcpy(state->putBuff+state->usedBuff,&_writePropertyProperty,sizeof(uint16_t));
			state->usedBuff+=sizeof(uint16_t);
			//
			if (state->isBottomTable){				
				// also write crc32, which is already stored in little endian format
				memcpy(state->putBuff+state->usedBuff,&(state->cachedMeta[state->curSourceIndex].crc32),sizeof(uint32_t));
				state->usedBuff+=sizeof(uint32_t);
				// also write position
				uint64_t _fixedHash = htole64(state->cachedMeta[state->curSourceIndex].posInFile);
				memcpy(state->putBuff+state->usedBuff,&_fixedHash,sizeof(uint64_t));
				state->usedBuff+=sizeof(uint64_t);
			}
			break;
		case WRITESTATE_FILENAME:
			resetBuffState(state,WRITESTATE_COMMENT);
			if (state->user.getFilenameFunc(state->curSourceIndex,&(state->putBuff),state->user.userData)){
				return -2;
			}
			state->usedBuff=strlen(state->putBuff);
			break;
		case WRITESTATE_COMMENT:
			resetBuffState(state,WRITESTATE_FILEDATA);
			if (state->user.getCommentFunc(state->curSourceIndex,&(state->putBuff),state->user.userData)){
				return -2;
			}
			state->usedBuff=strlen(state->putBuff);
			break;
		case WRITESTATE_FILEDATA:
			if (!state->isBottomTable){
				state->wstate=WRITESTATE_BITBYBIT;
				state->localProgress=0;
				state->getBitFunc=getFileBitAndHash;
				state->getBitFuncData=state;
				state->nextState=WRITESTATE_FINISHHASH;
				// default hash
				state->cachedMeta[state->curSourceIndex].crc32 = crc32(0L, Z_NULL, 0);
			}else{
				// skip file data in bottom metadata table
				_newState=WRITESTATE_GOTONEXTFILE;
				goto top;
			}
			break;
		case WRITESTATE_FINISHHASH:
			// done with file contents
			if (state->user.closeSourceFunc(state->curSourceIndex,state->curSourceData,state->user.userData)){
				return -2;
			}
			// verify that we read as many bytes as we expected to
			if (state->localProgress!=le64toh(state->cachedMeta[state->curSourceIndex].len)){
				printf("Wrong number of bytes written. wrote %ld expected %ld\n",state->localProgress,le64toh(state->cachedMeta[state->curSourceIndex].len));
				return 2;
			}
			// convert the hash we were working on before to little endian
			state->cachedMeta[state->curSourceIndex].crc32=htole32(state->cachedMeta[state->curSourceIndex].crc32);
			// write the hash
			resetBuffState(state,WRITESTATE_GOTONEXTFILE);
			memcpy(state->putBuff,&(state->cachedMeta[state->curSourceIndex].crc32),sizeof(uint32_t));
			state->usedBuff=sizeof(uint32_t);
			break;
		case WRITESTATE_GOTONEXTFILE:
			if (++state->curSourceIndex==state->numSources){ // if we're at the end
				if (!state->isBottomTable){
					_newState=WRITESTATE_TABLESTART;
					goto top;
				}else{
					state->wstate=WRITESTATE_DONE;
				}
			}else{
				// proceed to next file as normal
				_newState=WRITESTATE_FILEHEADER;
				goto top;
			}
			break;
		case WRITESTATE_TABLESTART:
			state->isBottomTable=1;
			state->curSourceIndex=0;
			resetBuffState(state,WRITESTATE_THISTIMESTAMP);
			// magic
			memcpy(state->putBuff,TABLEMAGIC,strlen(TABLEMAGIC));
			state->usedBuff=strlen(TABLEMAGIC);
			// count
			uint64_t _fixedFileCount = htole64(state->numSources);
			memcpy(state->putBuff+state->usedBuff,&_fixedFileCount,sizeof(uint64_t));
			state->usedBuff+=sizeof(uint64_t);
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
		char gotoNextState=0;
		size_t justWrote; // number of bytes you wrote in this iteration
		signed char ret=0;
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
			case WRITESTATE_DONE:
				ret=-1;
				break;
			case WRITESTATE_BITBYBIT:
			{
				signed char readRet = state->getBitFunc(state->getBitFuncData,dest,bytesRequested,&justWrote);
				state->localProgress+=justWrote;
				*numBytesWritten+=justWrote;
				if (readRet==-1){
					gotoNextState=1;
				}else{
					ret=readRet;
				}
			}
			break;
			default:
				puts("unimplemented state");
				return -1;
		}
		if (gotoNextState){
			state->filePos+=justWrote;
			bytesRequested-=justWrote;
			dest+=justWrote;
			if (initState(state,state->nextState)){
				return -2;
			}
			goto top;
		}
		state->filePos+=*numBytesWritten;
		return ret;
	}
}
struct userCallbacks* getCallbacks(struct compressState* s){
	return &(s->user);
}
void setTime(struct compressState* s, time_t _newTime){
	s->cachedTime=_newTime;
}
struct compressState* newState(size_t _numSources){
	struct compressState* state = malloc(sizeof(struct compressState));
	state->numSources=_numSources;
	state->cachedMeta=malloc(sizeof(struct fileMeta)*_numSources);
	// queue magic as first thing to do by using buffer write state that instantly ends
	resetBuffState(state,WRITESTATE_TOPMAGIC);
	state->usedBuff=0; // will cause to go to WRITESTATE_TOPMAGIC instantly
	state->curSourceIndex=0;
	state->isBottomTable=0;
	state->filePos=0;
	state->cachedTime=time(NULL); // programmer can change this variable right after this function call if he wishes
	return state;
}
