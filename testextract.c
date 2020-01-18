#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/sendfile.h>
#include <unistd.h>
#include <stdint.h>
#include <dirent.h>
#include <stdlib.h>
#include "formatInfo.h"
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
// read a string and if it's not what was expected then return -2
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
signed char read64(FILE* fp, uint64_t* _dest){
	freadSafe(_dest,1,sizeof(uint64_t),fp);
	*_dest = le64toh(*_dest);
	return 0;
}
signed char read16(FILE* fp, uint16_t* _dest){
	freadSafe(_dest,1,sizeof(uint16_t),fp);
	*_dest = le16toh(*_dest);
	return 0;
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
#define COPYBUFF 16000
signed char lowCopyFile(FILE* _sourceFp, const char* _destPath, char _canMakeDirs, size_t _totalBytes){
	int _outdesc = open(_destPath, O_WRONLY | O_CREAT, 0644);
	if (_outdesc!=-1){
		// faster linux specific function
		#if __linux__
		off_t _sourceCurPos;
		if ((_sourceCurPos = ftello(_sourceFp))==-1
			|| fflush(_sourceFp)!=0){
			exit(1);
		}
		int _srcDesc = fileno(_sourceFp);
		if (_srcDesc==-1 ||
			sendfile(_outdesc,_srcDesc,&_sourceCurPos,_totalBytes)==-1 ||
			fseek(_sourceFp,_sourceCurPos,SEEK_SET) ||
			close(_outdesc)){
			perror("file copy failure");
			exit(1);
		}
		#else
		FILE* _destfp = fdopen(_outdesc,"wb");
		char* _currentBit = malloc(COPYBUFF);
		size_t _numRead=COPYBUFF;
		while (_totalBytes!=0){
			if (_totalBytes<COPYBUFF){
				_numRead=_totalBytes;
			}
			freadSafe(_currentBit,1,_numRead,_sourceFp);
			if (fwrite(_currentBit,1,_numRead,_destfp)!=_numRead){
				fprintf(stderr,"wrote wrong number of bytes.\n");
				exit(1);
			}
			_totalBytes-=_numRead;
		}
		free(_currentBit);
		fclose(_destfp);
		#endif
	}else{
		// Make all directories that need to be made for the destination to work
		char _shouldRetry=0;
		if (_canMakeDirs){
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
				_shouldRetry=1;
				int i;
				for (i=0;i<_numMakeDirs;++i){
					_tempPath[strlen(_tempPath)]='/';
					if (mkdir(_tempPath,0777)){
						fprintf(stderr,"Failed to make directory %s\n",_tempPath);
						exit(1);
					}
				}
			}
			free(_tempPath);
		}
		if (_shouldRetry){
			lowCopyFile(_sourceFp,_destPath,0,_totalBytes);
		}else{
			fprintf(stderr,"Failed to open for writing %s\n",_destPath);
			exit(1);
		}
	}
	return 0;
}
char copyFile(FILE* _sourceFp, const char* _destPath, size_t _totalBytes){
	return lowCopyFile(_sourceFp,_destPath,1,_totalBytes);
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
		// skip timestamp
		safeSkipBytes(fp,sizeof(uint64_t));
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
		// read and then write file data
		printf("To %s\n",_fullPath);
		if (copyFile(fp,_fullPath,_fileLen)){
			return 1;
		}
		free(_fullPath);
		// skip crc32
		safeSkipBytes(fp,sizeof(uint32_t));
	}
	fclose(fp);
}
