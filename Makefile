CC = gcc
CFLAGS = -Wall -Wextra -g

all: drone operator commander

drone: src/drone.c src/common.h
	$(CC) $(CFLAGS) -o drone src/drone.c

operator: src/operator.c src/common.h
	$(CC) $(CFLAGS) -o operator src/operator.c

commander: src/commander.c src/common.h
	$(CC) $(CFLAGS) -o commander src/commander.c

clean:
	rm -f drone operator commander
