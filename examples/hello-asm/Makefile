# hello: a raw 65C02 binary at $0803, with ca65's debug file so ApplEm knows
# where it loads and names its labels. Build and Run (Command-B) runs this.

build/hello: hello.s
	mkdir -p build
	cl65 -t apple2enh -C apple2enh-asm.cfg --start-addr 0x0803 -g \
	    -Wl --dbgfile,build/hello.dbg -l build/hello.lst -o build/hello hello.s

clean:
	rm -rf build

.PHONY: clean
