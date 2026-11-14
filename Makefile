# Regenerate everything from spec/uisa.yaml and run the checks.
# Verilator's generated makefiles cannot live under a path containing spaces.
VMDIR ?= /tmp/uisa_obj_tb_$(USER)
GEN := build/uisa_pkg.sv build/uisa.h build/uisa_sem.inc doc/uisa.md build/vectors.hex

all: test

$(GEN): spec/uisa.yaml doc/intro.md tools/gen.py tools/uisa.py
	python3 tools/gen.py

gen: $(GEN)

# PLA flow coverage against a recorded NeXTSTEP histogram
doc/flows.md: spec/flows.yaml spec/uisa.yaml tools/flows.py tools/m68kdec.py data/ns22_boot_desktop.hist
	python3 tools/flows.py report data/ns22_boot_desktop.hist --top 45 -o $@

flows: doc/flows.md

test: $(GEN)
	python3 tests/selftest.py
	python3 tests/test_m68kdec.py
	python3 tools/flows.py check
	c++ -std=c++17 -O1 -Wall -Wextra -Ibuild -o build/test_cxx tests/test_cxx.cc
	./build/test_cxx
	python3 tools/gen_model.py >/dev/null
	c++ -std=c++17 -O1 -Ibuild -Imodel -o build/test_shadow tests/test_shadow.cc model/uisa_shadow.cc
	./build/test_shadow
	verilator --binary -Wall -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM --top-module tb_decode \
		--Mdir $(VMDIR) build/uisa_pkg.sv tests/tb_decode.sv -o tb_decode >/dev/null
	$(VMDIR)/tb_decode

clean:
	rm -rf build $(VMDIR)

.PHONY: all gen flows test clean
