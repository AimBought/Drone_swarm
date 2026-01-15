CC = gcc
CFLAGS = -Wall -Wextra -g
INC = -Iinclude

# Pliki Ÿród³owe
SRCS_COMM = src/ipc_wrapper.c
SRCS_DRONE = src/drone.c
SRCS_OP = src/operator.c
SRCS_CMD = src/commander.c

# Cele (pliki wynikowe)
all: drone operator commander

drone: $(SRCS_DRONE) $(SRCS_COMM)
	$(CC) $(CFLAGS) $(INC) -o drone $(SRCS_DRONE) $(SRCS_COMM)

operator: $(SRCS_OP) $(SRCS_COMM)
	$(CC) $(CFLAGS) $(INC) -o operator $(SRCS_OP) $(SRCS_COMM)

commander: $(SRCS_CMD) $(SRCS_COMM)
	$(CC) $(CFLAGS) $(INC) -o commander $(SRCS_CMD) $(SRCS_COMM)

clean:
	rm -f drone operator commander *.txt

rebuild: clean all