#include <stdio.h>
#include "../gc.h"

int main(){
  GarbageCollector gc;
  gc_start(&gc,__builtin_frame_address(0));
  int* a=gc_malloc(&gc,sizeof(int));
  printf("Allocated: %d\n",*a);
  a=NULL;
  gc_run(&gc);
  gc_stop(&gc);
  return 0;
}
