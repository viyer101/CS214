#include <stdlib.h>
#include "mymalloc.h"

int main() {
  malloc(1);     // rounds to 8
  malloc(9);     // rounds to 16
  return 0;
}