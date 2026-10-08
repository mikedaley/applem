# hello: a C program for the enhanced //e, linked by cl65 into an AppleSingle
# file. Build and Run (Command-B) in ApplEm boots it from ProDOS.

build/hello: hello.c
	mkdir -p build
	cl65 -t apple2enh -O -g -Wl --dbgfile,build/hello.dbg -o build/hello hello.c

clean:
	rm -rf build *.o

.PHONY: clean
