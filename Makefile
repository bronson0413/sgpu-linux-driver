# Top-level entry points. Each target calls the script or build that does the work.
WORK ?= $(HOME)/work
CXXFLAGS := -std=c++20 -O2 -Wall -Wextra -Werror -static -pthread -Iinclude -Iruntime

.PHONY: all setup driver mychar tests check image vmtest kcsan mutation bench fuzz clean

all: driver mychar tests

setup:
	@test -e $(WORK)/mychar || ln -s $(CURDIR)/mychar $(WORK)/mychar
	@echo "mychar: $$(readlink -f $(WORK)/mychar)"

driver:
	$(MAKE) -C driver

mychar:
	$(MAKE) -C mychar CC=gcc-14
	gcc -O2 -Wall -Wextra -Werror -static -pthread -o mychar/test_mychar mychar/test_mychar.c

tests: tests/test_sgpu tests/bench_sgpu

tests/%: tests/%.cpp runtime/sgpu.hpp include/sgpu_uapi.h
	g++ $(CXXFLAGS) -o $@ $<

check:
	perl $(WORK)/linux-lockdep/scripts/checkpatch.pl --no-tree --strict -f driver/sgpu.c mychar/mychar.c || true
	python3 -I tools/test_fuzzlog.py

image: all
	bash scripts/make_image.sh

vmtest:
	bash scripts/vm_test.sh 60

kcsan:
	bash scripts/kcsan_test.sh

mutation:
	bash scripts/mutation_test.sh

bench:
	bash scripts/bench.sh

fuzz:
	bash scripts/fuzz.sh 3600

clean:
	$(MAKE) -C driver clean
	$(MAKE) -C mychar clean
	rm -f tests/test_sgpu tests/bench_sgpu mychar/test_mychar
