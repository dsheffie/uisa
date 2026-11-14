# Regenerate everything from spec/uisa.yaml and run the checks.
# Verilator's generated makefiles cannot live under a path containing spaces.
VMDIR ?= /tmp/uisa_obj_tb_$(USER)
GEN := build/uisa_pkg.sv build/uisa.h build/uisa_sem.inc doc/uisa.md build/vectors.hex

all: test

$(GEN): spec/uisa.yaml doc/intro.md tools/gen.py tools/uisa.py
	python3 tools/gen.py

gen: $(GEN)

test: $(GEN)
	python3 tests/selftest.py
	python3 tests/test_m68kdec.py
	c++ -std=c++17 -O1 -Wall -Wextra -Ibuild -o build/test_cxx tests/test_cxx.cc
	./build/test_cxx
	verilator --binary -Wall -Wno-UNUSEDSIGNAL -Wno-UNUSEDPARAM --top-module tb_decode \
		--Mdir $(VMDIR) build/uisa_pkg.sv tests/tb_decode.sv -o tb_decode >/dev/null
	$(VMDIR)/tb_decode

clean:
	rm -rf build $(VMDIR)

.PHONY: all gen test clean
