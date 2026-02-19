#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>


#ifndef REALMALLOC
#include "mymalloc.h"
#endif

#ifndef LEAK
#define LEAK 0
#endif

#define MEMSIZE 4096
#define HEADERSIZE 8
#define OBJECTS 64
#define OBJSIZE (MEMSIZE / OBJECTS - HEADERSIZE)

int 
main (int argc, char **argv)
{
	char *obj[OBJECTS];
	int i, j, errors = 0;
	
	for (i = 0; i < OBJECTS; i++) {
		obj[i] = malloc (OBJSIZE);
		if (obj[i] == NULL) {
		    printf ("Unable to allocate object %d\n", i);
		    exit (EXIT_FAILURE);
		}
	}
	
	for (i = 0; i < OBJECTS; i++) {
		memset (obj[i], i, OBJSIZE);
	}
	
	for (i = 0; i < OBJECTS; i++) {
		for (j = 0; j < OBJSIZE; j++) {
			if (obj[i][j] != i) {
				errors++;
				printf ("Object %d byte %d incorrect: %d\n", i, j, obj[i][j]);
			}
		}
	}

	if (!LEAK) {
	    for (i = 0; i < OBJECTS; i++) {
		free (obj[i]);
	    }
	}
	
	printf("%d incorrect bytes\n", errors);
	
	return EXIT_SUCCESS;
}
