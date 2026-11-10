# Lab 2 build. `make` builds build/see (your tool), build/run and build/grade.
#
#   make grade              score build/see on all public tasks
#   make grade BLOCK=loop   on the tasks of one block (ids in tasks/blocks.json)
#   make grade J=4 TIMEOUT=10
#
# Z3 comes from the z3-solver Python package (pip install -r requirements.txt),
# which ships the C++ header and library. Set Z3_DIR to use another installation:
# headers in $(Z3_DIR)/include, libz3 in $(Z3_DIR)/lib.

CXX      ?= g++
CXXFLAGS ?= -std=c++17 -O2 -g -Wall -Wextra
PYTHON   ?= python3

Z3_DIR ?= $(shell $(PYTHON) -c "import z3, os; print(os.path.dirname(z3.__file__))" 2>/dev/null)
ifneq ($(Z3_DIR),)
Z3_CFLAGS := -I$(Z3_DIR)/include
Z3_LIBS   := -L$(Z3_DIR)/lib -lz3 -Wl,-rpath,$(Z3_DIR)/lib
else
Z3_LIBS   := -lz3
endif

MINIC_OBJ := $(patsubst %.cpp,build/obj/%.o,$(wildcard minic/*.cpp))
SEE_OBJ   := $(patsubst %.cpp,build/obj/%.o,$(wildcard see/*.cpp))

.PHONY: all grade handin clean

all: build/see build/run build/grade

build/see: $(SEE_OBJ) $(MINIC_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(Z3_LIBS)

build/run: build/obj/tools/run.o $(MINIC_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

build/grade: build/obj/tools/grade.o $(MINIC_OBJ)
	$(CXX) $(CXXFLAGS) -pthread -o $@ $^

# Command-line entry to the scoring code; used by the course's own tests.
build/judge: build/obj/tools/judge.o $(MINIC_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $^

build/obj/see/%.o: see/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(Z3_CFLAGS) -Iminic -MMD -MP -c -o $@ $<

build/obj/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -Iminic -MMD -MP -c -o $@ $<

-include $(MINIC_OBJ:.o=.d) $(SEE_OBJ:.o=.d) build/obj/tools/run.d build/obj/tools/grade.d build/obj/tools/judge.d

J       ?= 4
TIMEOUT ?= 60
grade: all
	./build/grade -j $(J) --timeout $(TIMEOUT) $(if $(BLOCK),--block $(BLOCK))

handin:
	rm -f handin.zip
	zip -q handin.zip see/*.cpp see/*.h see/*.hpp 2>/dev/null || zip -q handin.zip see/*.cpp see/*.h
	@echo "handin.zip created with these source files:"
	@unzip -l handin.zip | sed -n '4,$$p' | head -n -2

clean:
	rm -rf build handin.zip
