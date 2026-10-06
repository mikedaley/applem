# The profiler demo: a raw 65C02 binary at $6000, with ca65's debug file so
# ApplEm names every routine. Build and Run (Command-B) in ApplEm runs this.

build/demo: demo.s
	mkdir -p build
	cl65 -t apple2enh -C apple2enh-asm.cfg --start-addr 0x6000 -g \
	    -Wl --dbgfile,build/demo.dbg -l build/demo.lst -o build/demo demo.s

clean:
	rm -rf build

.PHONY: clean
