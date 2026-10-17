CC = gcc
CFLAGS = -Wall -Wextra -pthread -O2
TARGET = server
OBJS = main.o server.o client_manager.o http.o

# Declara alvos que não geram arquivos diretamente no disco
.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

run: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(OBJS) $(TARGET)