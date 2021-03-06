// See LICENSE.woarc file for license information
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <unistd.h>
#include <stdint.h>
#include <dirent.h>
#include <stdlib.h>
#include <zlib.h>
#include <sys/time.h>
#include "woarcFormatInfo.h"
//
const char* findCharBackwards(const char* _startHere, const char* _endHere, int _target){
	do{
		if (_startHere[0]==_target){
			return _startHere;
		}
		--_startHere;
	}while(_startHere>_endHere);
	return NULL;
}
//
signed char freadSafe(void *ptr, size_t size, size_t nmemb, FILE *stream){
	if (fread(ptr,size,nmemb,stream)!=nmemb){
		fprintf(stderr,"failed to read %ld x %ld bytes\n",size,nmemb);
		exit(1);
	}
	return 0;
}
signed char safeSkipBytes(FILE* fp, size_t _numForward){
	if (fseek(fp,_numForward,SEEK_CUR)){
		fprintf(stderr,"seek error\n");
		exit(1);
	}
	return 0;
}
// read a string and if it's not what was expected then exit
signed char readExpectedString(FILE* fp, const char* _expected){
	int _len = strlen(_expected);
	char _readBytes[_len];
	freadSafe(_readBytes,1,_len,fp);
	if (memcmp(_readBytes,_expected,_len)!=0){
		fprintf(stderr,"error: expected to read %s\n",_expected);
		exit(1);
	}
	return 0;
}
void read64(FILE* fp, uint64_t* _dest){
	freadSafe(_dest,1,sizeof(uint64_t),fp);
	*_dest = le64toh(*_dest);
}
void read32(FILE* fp, uint32_t* _dest){
	freadSafe(_dest,1,sizeof(uint32_t),fp);
	*_dest = le32toh(*_dest);
}
void read16(FILE* fp, uint16_t* _dest){
	freadSafe(_dest,1,sizeof(uint16_t),fp);
	*_dest = le16toh(*_dest);
}
char dirExists(const char* _passedPath){
	DIR* _tempDir = opendir(_passedPath);
	if (_tempDir!=NULL){
		closedir(_tempDir);
		return 1;
	}/*else if (ENOENT == errno)*/
	return 0;
}
//
// Make all directories that need to be made for the destination to work
// returns 1 if actually made any folders
char makeRequiredDirs(const char* _destPath){
	char _ret=0;
	char* _tempPath = strdup(_destPath);
	int _numMakeDirs=0;
	while(1){
		char* _possibleSeparator=(char*)findCharBackwards(&(_tempPath[strlen(_tempPath)-1]),_tempPath,'/');
		if (_possibleSeparator!=NULL && _possibleSeparator!=_tempPath){
			_possibleSeparator[0]='\0';
			if (dirExists(_tempPath)){ // When the directory that does exist is found break to create the missing ones in order.
				break;
			}else{
				++_numMakeDirs;
			}
		}else{
			break;
		}
	}
	if (_numMakeDirs>0){
		_ret=1;
		int i;
		for (i=0;i<_numMakeDirs;++i){
			_tempPath[strlen(_tempPath)]='/';
			if (mkdir(_tempPath,0777)){
				fprintf(stderr,"Failed to make directory %s\n",_tempPath);
				perror(NULL);
				exit(1);
			}
		}
	}
	free(_tempPath);
	return _ret;
}
#define COPYBUFF 16000
// returns the hash
uLong copyFile(FILE* _sourceFp, const char* _destPath, size_t _totalBytes){
	char _canMakeDirs=1;
top:
	;
	FILE* _destfp = fopen(_destPath,"wb");
	if (_destfp!=NULL){
		uLong _retHash = crc32(0L, Z_NULL, 0);
		char* _currentBit = malloc(COPYBUFF);
		size_t _numRead=COPYBUFF;
		while (_totalBytes!=0){
			if (_totalBytes<COPYBUFF){
				_numRead=_totalBytes;
			}
			freadSafe(_currentBit,1,_numRead,_sourceFp);
			_retHash = crc32_z(_retHash,(const Bytef*)_currentBit,_numRead);
			if (fwrite(_currentBit,1,_numRead,_destfp)!=_numRead){
				fprintf(stderr,"wrote wrong number of bytes.\n");
				exit(1);
			}
			_totalBytes-=_numRead;
		}
		free(_currentBit);
		if (fclose(_destfp)==EOF){
			perror("lowCopyFile fclose");
		}
		return _retHash;
	}else{
		// Make all directories that need to be made for the destination to work
		char _shouldRetry=(_canMakeDirs && makeRequiredDirs(_destPath));
		if (_shouldRetry){
			_canMakeDirs=0;
			goto top;
		}else{
			fprintf(stderr,"Failed to open for writing %s\n",_destPath);
			exit(1);
		}
	}
}
//
int main(int argc, char** args){
	if (argc==1 || argc>3){
		fprintf(stderr,"%s <archive file> [out dir]\n",args[0]);
		return 1;
	}
	char* _outRoot;
	if (argc==3){
		_outRoot=args[2];
		// force it to end in a slash
		if (_outRoot[strlen(_outRoot)-1]!='/'){
			_outRoot=malloc(strlen(_outRoot)+2);
			strcpy(_outRoot,args[2]);
			_outRoot[strlen(_outRoot)]='/';
			_outRoot[strlen(_outRoot)]='\0';
		}
	}else{
		_outRoot="./";
	}
	FILE* fp = fopen(args[1],"rb");
	if (!fp){
		fprintf(stderr,"failed to open %s\n",args[1]);
		exit(1);
	}
	// read top magic
	readExpectedString(fp,TOPMAGIC);
	// read version
	if (getc(fp)!=WRITEVERSION){
		fprintf(stderr,"bad version\n");
		return 1;
	}
	// skip timestamp
	safeSkipBytes(fp,sizeof(uint64_t));
	for (;;){
		char _magic[strlen(MFHMAGIC)];
		freadSafe(_magic,1,strlen(MFHMAGIC),fp);
		// check if we're out of files yet by checking the magic
		if (memcmp(_magic,MFHMAGIC,strlen(MFHMAGIC))!=0){
			if (memcmp(_magic,TABLEMAGIC,strlen(MFHMAGIC))==0){
				break;
			}
			fprintf(stderr,"bad magic read\n");
			return 1;
		}
		uint64_t _fileLen;
		uint16_t _nameLen;
		uint16_t _commentLen;
		read64(fp,&_fileLen);
		read16(fp,&_nameLen);
		read16(fp,&_commentLen);
		// timestamp
		uint64_t _modifiedTime;
		read64(fp,&_modifiedTime);
		printf("modified at %ld\n",_modifiedTime);
		// property
		unsigned char prop;
		freadSafe(&prop,1,1,fp);
		// propertyProperty
		uint16_t propProp;
		read16(fp,&propProp);
		// read variable sized name and comment
		char* _curFilename=malloc(_nameLen+1);
		freadSafe(_curFilename,1,_nameLen,fp);
		_curFilename[_nameLen]='\0';
		char* _curComment=malloc(_commentLen+1);
		freadSafe(_curComment,1,_commentLen,fp);
		_curComment[_commentLen]='\0';
		// add extract root dest
		char* _fullPath = malloc(strlen(_curFilename)+strlen(_outRoot)+1);
		strcpy(_fullPath,_outRoot);
		strcat(_fullPath,_curFilename);
		uint32_t _gotHash;
		// ready and pointing to the start of the file data
		printf("To %s\n",_fullPath);
		if (prop==FILEPROP_NORMAL){
			// read and then write file data
			_gotHash = copyFile(fp,_fullPath,_fileLen);
		}else if (prop==FILEPROP_LINK){
			// read and hash the link destination
			char* _linkDest;
			if (propProp==0){ // is a relative link, prepend the thing
				_linkDest=malloc(strlen(_outRoot)+_fileLen+1);
				strcpy(_linkDest,_outRoot);
			}else{ // it's an absolute link
				_linkDest = malloc(_fileLen+1);
				_linkDest[0]='\0';
			}
			char* _readPos=&_linkDest[strlen(_linkDest)];
			freadSafe(_readPos,1,_fileLen,fp);
			_gotHash = crc32_z(crc32_z(0L,Z_NULL,0),(const Bytef*)_readPos,_fileLen);
			// make link
			if (symlink(_linkDest,_fullPath)){
				// if it fails, perhaps we need to make folders first
				if (!makeRequiredDirs(_fullPath) || symlink(_linkDest,_fullPath)){
					perror("symlink creation");					
				}
			}
			free(_linkDest);
		}else{
			fprintf(stderr,"bad file property %d\n",prop);
			exit(1);
		}
		// set timestamp
		struct timeval _stamps[2];
		_stamps[0].tv_sec=_modifiedTime;
		_stamps[0].tv_usec=0;
		_stamps[1]=_stamps[0];
		if (lutimes(_fullPath,_stamps)){
			perror("failed to set file modified time");
		}
		free(_fullPath);
		// check crc32
		uint32_t _expected;
		read32(fp,&_expected);
		if (_expected!=_gotHash){
			fprintf(stderr,"hash failed. expected %08X got %08X",_expected,_gotHash);
		}
	}
	fclose(fp);
}
