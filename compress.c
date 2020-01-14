// todo - maybe just make a state "go to in nextState"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <sys/stat.h> // required for FILE implementation

enum writeState{
	WRITESTATE_UNKNOWN=0,
	WRITESTATE_PUTBUFF, // write whatever is in the buffer and then proceed to nextState
	WRITESTATE_TOPMAGIC,
	WRITESTATE_FILEHEADER,
	WRITESTATE_FILEDATA,
	WRITESTATE_FILENAME,
	WRITESTATE_COMMENT,
	WRITESTATE_MFHMAGIC, // stands for "middle file header magic"
	WRITESTATE_TABLESTART,
	WRITESTATE_DONE,
};

struct compressState{
	// data given
	size_t numSources;
	void** sources;
	char** filenames;
	// data calculated and saved for later
	size_t curSource;
	signed char wstate;
	signed char nextState; // used from WRITESTATE_PUTBUFF and WRITESTATE_COMMENT
	//size_t pos; // internal use only. this variable may be a lie if data has been put into buffer
	uint64_t localProgress; // in WRITESTATE_PUTBUFF represents next char to be written

	char buff[256]; // temporary buffer size. will resize it to be the size of the biggest thing that goes in it.
	int usedBuff;
};

#define MFHMAGIC "WARCFILE"
#define TOPMAGIC "WARCARCHIVESUKI"
#define WRITEVERSION 1

signed char getFileSize(void* src, size_t* ret){
	struct stat st;
	if (fstat(fileno(src),&st)){
		return 1;
	}else{
		*ret=st.st_size;
		return 0;
	}
}

void resetBuffState(struct compressState* state, signed char _nextState){
	state->wstate=WRITESTATE_PUTBUFF;
	state->nextState=_nextState;
	state->localProgress=0;
}
void buffAsMagic(struct compressState* state){
	memcpy(state->buff,TOPMAGIC,strlen(TOPMAGIC));
	state->buff[strlen(TOPMAGIC)]=WRITEVERSION;
	state->usedBuff=strlen(TOPMAGIC)+1;
	resetBuffState(state,WRITESTATE_MFHMAGIC);
}

// returns 0 if worked, else on error
signed char initNextState(struct compressState* state){
	state->localProgress=0;
	switch(state->nextState){
		case WRITESTATE_TOPMAGIC:
			buffAsMagic(state);
			break;
		case WRITESTATE_MFHMAGIC:
			resetBuffState(state,WRITESTATE_FILEHEADER);
			strcpy(state->buff,MFHMAGIC);
			state->usedBuff=strlen(state->buff);
			break;
		case WRITESTATE_COMMENT:
			// TODO - proceed to WRITESTATE_FILEDATA or WRITESTATE_TABLESTART depending on the variable
			// THIS CODE IS FOR AFTER COMPLETION OF WRITESTATE_COMMENT
			// THIS CODE IS FOR AFTER COMPLETION OF WRITESTATE_COMMENT
			// THIS CODE IS FOR AFTER COMPLETION OF WRITESTATE_COMMENT
			if (++state->curSource==state->numSources){
				// normally, proceed to footer table. in this case, just be done.
				state->wstate=WRITESTATE_DONE;
			}else{
				state->wstate=WRITESTATE_MFHMAGIC;
				// temporary hack?
				state->nextState=state->wstate;
				return initNextState(state);
			}
			break;
		case WRITESTATE_FILEHEADER:
			// prepare file buffer here
			resetBuffState(state,WRITESTATE_COMMENT);
			strcpy(state->buff,"HEADERGOESHERE");
			state->usedBuff=strlen(state->buff);
			break;
		default:
			printf("invalid next state %d\n",state->nextState);
			return -1;
	}
	return 0;
}
/*
-2: error. 		numBytesWritten not updated
-1: done. 		numBytesWritten not updated
 0: happy end. 	numBytesWritten is updated.
*/
signed char makeMoreArchive(struct compressState* state, char* dest, size_t bytesRequested, size_t* numBytesWritten){
	*numBytesWritten=0;
top:
	switch(state->wstate){
		case WRITESTATE_PUTBUFF:
		{
			char* curSource = state->buff+state->localProgress;
			// if we have enough left to fill the dest
			if (state->localProgress+bytesRequested<=state->usedBuff){
				memcpy(dest,curSource,bytesRequested);
				state->localProgress+=bytesRequested;
				*numBytesWritten+=bytesRequested;
			}else{
				// write what's left
				size_t numWrite = state->usedBuff-state->localProgress;
				memcpy(dest,curSource,numWrite);
				// continue on
				*numBytesWritten+=numWrite;
				bytesRequested-=numWrite;
				dest+=numWrite;
				if (initNextState(state)){
					return -2;
				}
				goto top;
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
		default:
			puts("unimplemented state");
			return -1;
	}
	return 0;
}

void prepareState(struct compressState* state, size_t _numSources, void** _sources, char** _filenames){
	state->sources=_sources;
	state->filenames=_filenames;
	state->numSources=_numSources;
	// queue magic as first thing to do by using buffer write state that instantly ends
	resetBuffState(state,WRITESTATE_TOPMAGIC);
	state->usedBuff=0; // will cause to go to WRITESTATE_TOPMAGIC instantly
	state->curSource=0;
}

int main(int argc, char** args){
	struct compressState s;
	
	void** sources = malloc(sizeof(FILE*)*3);
	sources[0]=fopen("/tmp/a","rb");
	sources[1]=fopen("/tmp/b","rb");
	sources[2]=fopen("/tmp/c","rb");
	char** names = malloc(sizeof(char*)*3);
	names[0]="a";
	names[1]="b";
	names[2]="c";
	
	prepareState(&s,3,sources,names);

	// write archive to file
	FILE* dest = fopen("/tmp/outarc","wb");
	char writeBuff[256];
	size_t gotBytes;
	signed char lastRet;
	while(!(lastRet=makeMoreArchive(&s,writeBuff,256,&gotBytes))){
		if (fwrite(writeBuff,1,gotBytes,dest)!=gotBytes){
			lastRet=2;
			break;
		}
	}
	writeBuff[gotBytes]='\0';
	printf("%s\n",writeBuff);
	if (lastRet==-2){
		printf("error!\n");
	}
	fclose(dest);
}
