# Testing guide for Garbage Collector

## **Compilation Instructions**
To compile the project, use the followwing command:

gcc test_gc.c path_to_gc.c/gc.c -o test_gc

To check for memory leaks using Valgrind:

valgrind --leak-check=full --track-origin=yes ./test_gc 
