.PHONY: all clean

all:
	gcc -o code main.c buddy.c

clean:
	rm -f code test