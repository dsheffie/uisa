# q800sim beside this checkout: its SoftFloat-3 submodule is the FPU tests' reference
Q800SIM ?= ../q800sim
# Regenerate everything from spec/uisa.yaml and run the checks.
# Verilator's generated makefiles cannot live under a path containing spaces.
VMDIR ?= /tmp/uisa_obj_tb_$(USER)
# ucc flags for millicode written in C: -O0 greedy register allocation for
# everyday builds, -O2 the z3 search for deploy builds (`make release`)
UCCFLAGS ?= -O0
export UCCFLAGS
# Previous's source: tests/softfloat compares the FPU millicode with its SoftFloat
PREVIOUS_SRC ?= $(HOME)/code/previous/src
export PREVIOUS_SRC
GEN := build/uisa_pkg.sv build/uisa.h build/uisa_sem.inc doc/uisa.md build/vectors.hex

all: test

$(GEN): spec/uisa.yaml doc/intro.md tools/gen.py tools/uisa.py
	python3 tools/gen.py

gen: $(GEN)

# the guest-op semantics (spec guest_ops `sem`) as SystemVerilog, for an RTL
# implementation (superdupersparc's `make uisa-gen` copies it)
build/uisa_gop.sv: spec/uisa.yaml tools/gen_sv.py tools/gen_smt.py tools/uisa.py
	python3 tools/gen_sv.py

gen-sv: build/uisa_gop.sv

.PHONY: gen-sv

# PLA flow coverage against a recorded NeXTSTEP histogram
doc/flows.md: spec/flows.yaml spec/uisa.yaml tools/flows.py tools/m68kdec.py data/ns22_boot_desktop.hist
	python3 tools/flows.py report data/ns22_boot_desktop.hist --top 45 -o $@

flows: doc/flows.md

# the PDF reference manual (needs pdflatex with tikz, listings, enumitem,
# fancyhdr, titlesec, changepage, longtable and hyperref); not part of `test`
doc/uisa.pdf: spec/uisa.yaml spec/flows.yaml doc/intro.md tools/gen_pdf.py tools/uisa.py tools/flows.py tools/m68kdec.py
	python3 tools/gen_pdf.py

pdf: doc/uisa.pdf

# the millicode C compiler (compiler/, CMake; needs flex, bison and z3)
compiler:
	cmake -S compiler -B build/compiler -DCMAKE_BUILD_TYPE=RelWithDebInfo >/dev/null
	cmake --build build/compiler -j8 >/dev/null

test: $(GEN) compiler
	python3 tests/selftest.py
	python3 tests/test_m68kdec.py
	python3 tools/flows.py check
	c++ -std=c++17 -O1 -Wall -Wextra -Ibuild -o build/test_cxx tests/test_cxx.cc
	./build/test_cxx
	python3 compiler/tests/test_ucc.py
	python3 tests/softfloat/test_fx80.py 20000
	python3 tests/fpu/test_fpu.py 20000
	$(MAKE) -C $(Q800SIM)/softfloat/build/Linux-x86_64-GCC >/dev/null
	c++ -std=c++17 -O1 -Imillicode -I$(Q800SIM)/softfloat/source/include -DSOFTFLOAT_FAST_INT64 -DLITTLEENDIAN=1 -o build/test_fpd_num \
		tests/fpd/test_num.cc $(Q800SIM)/softfloat/build/Linux-x86_64-GCC/softfloat.a
	./build/test_fpd_num 100000
	python3 tools/gen_model.py >/dev/null
	python3 tools/gen_millicode.py >/dev/null
	python3 tools/check_millicode.py -q
	python3 tests/test_check_millicode.py
	c++ -std=c++17 -O1 -Ibuild -Imodel -o build/test_shadow tests/test_shadow.cc model/uisa_shadow.cc model/uisa_machine.cc
	./build/test_shadow
	c++ -std=c++17 -O1 -Ibuild -Imodel -o build/test_engine tests/test_engine.cc model/uisa_engine.cc model/uisa_machine.cc
	./build/test_engine
	verilator --binary -Wall -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM --top-module tb_decode \
		--Mdir $(VMDIR) build/uisa_pkg.sv tests/tb_decode.sv -o tb_decode >/dev/null
	$(VMDIR)/tb_decode

# deploy build: millicode compiled with the optimal (z3) register allocation
release:
	$(MAKE) UCCFLAGS=-O2 test

clean:
	rm -rf build $(VMDIR)

.PHONY: all gen flows pdf compiler test release clean
