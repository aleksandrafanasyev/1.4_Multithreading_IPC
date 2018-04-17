.PHONY : all clean

SHELL := /bin/sh
CC := gcc
CFLAGS := -g -Wall -Werror
LDFLAGS := -g -Wall -Werror
LIBS := -lpthread

all: main_A_B main_C
       
main_A_B: main_A_B.o
	$(CC) $(LDFLAGS) -o main_A_B

main_C: main_C.o
	$(CC) $(LDFLAGS) $(LIBS) -o main_C

main_A_B.o:main_A_B.c ipc.h
	$(CC) -c $(CFLAGS) -o main_A_B.o

main_C.o: main_C.c ipc.h
	$(CC) -c $(CFLAGS) -o main_C.o

clean:
	rm -f main_A_B
	rm - main_C
	rm -f *.o



