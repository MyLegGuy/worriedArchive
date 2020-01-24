#define _XOPEN_SOURCE 500 // enable nftw
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ftw.h>
#include <time.h>
#include <sys/stat.h>
#include <woarcassemble.h>
//////////////////////
// file callbacks
//////////////////////
struct fileCallInfo{
	char** filenames; // absolute filenames
	char* rootDir; // ends in a slash. chop everything off the filename past this. 
};
signed char getFileSize(FILE* fp, size_t* ret){
	struct stat st;
	if (fstat(fileno(fp),&st)){
		return -2;
	}
	*ret=st.st_size;
	return 0;
}
signed char myInitSource(size_t i, struct fileMeta* infoDest, void** srcDest, void* _userData){
	if (!(*srcDest=fopen(((struct fileCallInfo*)_userData)->filenames[i],"rb"))){
		return -2;
	}
	size_t _gotLen;
	if (getFileSize(*srcDest,&_gotLen)){
		return -2;
	}
	infoDest->len=_gotLen;
	infoDest->lastModified=0;
	return 0;
}
signed char myCloseSource(size_t i, void* _closeThis, void* _userData){
	return fclose(_closeThis)==0 ? 0 : -2;
}
signed char myGetFilename(size_t i, char** dest, void* _userData){
	struct fileCallInfo* _info = _userData;
	*dest=_info->filenames[i]+strlen(_info->rootDir);
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
signed char myGetFileProp(size_t i, uint8_t* _dest, uint16_t* _dest2, void* _userData){
	*_dest=0;
	*_dest2=0;
	return 0;
}
///////////////////////
// CLI
///////////////////////
char** globalFiles=NULL;
size_t curUsedFiles=0;
size_t curMaxFiles=0;

char* rootDirAdd;
int nftwAdd(const char *fpath, const struct stat *sb, int typeflag, struct FTW* ftwbuf){
	if (typeflag==FTW_F){
		if (strncmp(fpath,rootDirAdd,strlen(rootDirAdd))!=0){
			printf("somehow got to a file that's not in the folder: %s\n",fpath);
			exit(1);
		}
		// increase files array if needed
		if (curUsedFiles>=curMaxFiles){			
			curMaxFiles+=10;
			if (!(globalFiles=realloc(globalFiles,sizeof(char*)*curMaxFiles))){
				puts("out of memory");
				exit(1);
			}
		}
		globalFiles[curUsedFiles++]=strdup(fpath);
	}
	return 0;
}
#define TESTCHUNKSIZE 500
int main(int argc, char** args){
	if (argc!=3 || (argc==3 && args[1][0]=='\0')){
		printf("%s <folder path> <out archive file>\n",args[0]);
		return 1;
	}
	rootDirAdd = args[1];
	// force it to have a slash at the end
	if (rootDirAdd[strlen(rootDirAdd)-1]!='/'){
		int _cachedLen=strlen(rootDirAdd);
		rootDirAdd = malloc(_cachedLen+2);
		memcpy(rootDirAdd,args[1],_cachedLen);
		rootDirAdd[_cachedLen]='/';
		rootDirAdd[_cachedLen+1]='\0';
	}
	// populate file list
	if (nftw(rootDirAdd,nftwAdd,5,0)==-1){
		puts("nftw error");
		exit(1);
	}
	if (curUsedFiles==0){
		puts("no files");
		return 1;
	}
	// init archive maker
	struct fileCallInfo* i = malloc(sizeof(struct fileCallInfo));
	i->filenames=globalFiles;
	i->rootDir=rootDirAdd;
	struct compressState* s = newState(curUsedFiles);
	struct userCallbacks* c = getCallbacks(s);
	c->userData=i; // see the usage of the passed userdata in the source open functions
	c->initSourceFunc=myInitSource;
	c->closeSourceFunc=myCloseSource;
	c->getFilenameFunc=myGetFilename;
	c->getCommentFunc=myGetComment;
	c->getSourceData=mygetDataFunc;
	c->getPropFunc=myGetFileProp;
	// write archive to file
	FILE* dest = fopen(args[2],"wb");
	if (!dest){
		perror("failed to open output file");
		return 1;
	}
	char writeBuff[TESTCHUNKSIZE];
	size_t gotBytes;
	signed char lastRet;
	while((lastRet=makeMoreArchive(s,writeBuff,TESTCHUNKSIZE,&gotBytes))!=-2){
		if (fwrite(writeBuff,1,gotBytes,dest)!=gotBytes){
			lastRet=-2;
			break;
		}
		if (lastRet==-1){
			break;
		}
	}
	if (lastRet==-2){
		puts("makeMoreArchive returned error");
	}
	fclose(dest);
}
