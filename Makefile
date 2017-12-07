CFLAGS := -Wall -O2

prompt: prompt.o
	cc -O2 -o $@ $^

clean:
	rm -f *.o prompt
