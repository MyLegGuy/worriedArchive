// See LICENSE.woarc file for license information
#define TOPMAGIC "WORRIEDARCHIVEMAGIC"
#define WRITEVERSION 1
#define MFHMAGIC "WOARCFILE" // middle file header
#define TABLEMAGIC "WORRIEDTABLESTART"
#define BGFMAGIC "WOARCENTRY" // bottom file header

#define FILEPROP_NORMAL 0
#define FILEPROP_LINK 1
#define FILEPROP_PARTIAL 2
#define FILEPROP_PARTIALLAST 3

// metadata space used with an archive with 0 files
#define WOARCBASEMETADATASPACE (				\
		strlen(TOPMAGIC) +	/* section A */		\
		1 + /*...*/								\
		8 + /* section B */						\
		strlen(TABLEMAGIC) + /* section H */	\
		8 + /*...*/								\
		8  /* section B */						\
		)
// lower bound additional metadata space used by 1 file
#define WOARCSINGLEFILEBASEOVERHEAD (			\
		strlen(MFHMAGIC) + /* section C */		\
		(8 + /* section D */					\
		 2 +									\
		 2 +									\
		 8 +									\
		 1 +									\
		 2										\
			)*2 + /* section D used twice */	\
		4 + /* section G */						\
		strlen(BGFMAGIC) + /* section I */		\
		4 + /* section J*/						\
		8										\
		)
// more additional metadata space used when you include a filename or comment of this length
#define WOARCFILENAMEMETADATASPACE(len) (		\
		(len)*2 /* section e is used twice */	\
		)
