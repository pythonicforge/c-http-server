CC = gcc
CFLAGS = -Wall -Wextra -std=c11

TARGET = server

SRCS = server.c directory_reader.c
OBJS = $(SRCS:.c=.o)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
