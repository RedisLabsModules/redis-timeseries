
ROOT=..
MK.pyver:=3

ifeq ($(VG),1)
override VALGRIND:=1
export VALGRIND
endif

ifeq ($(VALGRIND),1)
override DEBUG:=1
export DEBUG
endif

include $(ROOT)/deps/readies/mk/main

#----------------------------------------------------------------------------------------------  

define HELP
make setup         # install packages required for build
make fetch         # download and prepare dependant modules

make build
  DEBUG=1          # build debug variant
  VARIANT=name     # use a build variant 'name'
  PROFILE=1        # enable profiling compile flags (and debug symbols) for release type
                   # You can consider this as build type release with debug symbols and -fno-omit-frame-pointer
  DEPS=1           # also build dependant modules
  COV=1            # perform coverage analysis (implies debug build)
make clean         # remove binary files
  ALL=1            # remove binary directories
  DEPS=1           # also clean dependant modules

make deps          # build dependant modules
make all           # build all libraries and packages

make test          # run tests
  TEST=name        # run test matching 'name'
  TEST_ARGS="..."  # RLTest arguments
  GEN=0|1          # run general tests on a standalone Redis topology
  AOF=0|1          # run AOF persistency tests on a standalone Redis topology
  SLAVES=0|1       # run replication tests on standalone Redis topology
  CLUSTER=0|1      # run general tests on a OSS Redis Cluster topology
  GEARS=0|1        # also load RedisGears
  GEARS_PATH=path  # RedisGears module path
  COV=1            # perform coverage analysis
  VALGRIND|VG=1    # run specified tests with Valgrind

make pack          # build packages (ramp & dependencies)

make benchmarks        # run all benchmarks
  BENCHMARK=filename   # run benchmark specified by 'filename'
  BENCH_ARGS="..."     # redisbench_admin  extra arguments
endef

#----------------------------------------------------------------------------------------------  

MK_CUSTOM_CLEAN=1

BINDIR=$(BINROOT)/src
SRCDIR=.

#----------------------------------------------------------------------------------------------  

PREFIX ?= /usr/local
INSTALL_LIB:=$(PREFIX)/lib
INSTALL:=/usr/bin/install
PAKCAGE_NAME ?= $(BINROOT)/redistimeseries.{os}-{architecture}.latest.zip

# set environment variable RM_INCLUDE_DIR to the location of redismodule.h
SDK_DIR=$(ROOT)/deps/RedisModulesSDK
RMUTIL_LIBDIR=$(BINROOT)/rmutil
LIBRMUTIL=$(RMUTIL_LIBDIR)/librmutil.a
FAST_DOUBLE_PARSER_LIBDIR=$(BINROOT)/fast_double_parser_c
FAST_DOUBLE_PARSER_SRCDIR=$(SRCDIR)/fast_double_parser_c
LIBFAST_DOUBLE_PARSER=$(FAST_DOUBLE_PARSER_LIBDIR)/libfast_double_parser_c.a

#----------------------------------------------------------------------------------------------

TARGET=$(BINROOT)/redistimeseries.so
UNITTESTS_RUNNER=$(BINROOT)/unittests_runner

CC=gcc

CC_FLAGS = \
	-I$(SDK_DIR) \
	-I$(ROOT)/deps \
	-Wall \
	-fPIC \
	-pedantic \
	-std=gnu99 \
	-MMD -MF $(@:.o=.d) \
	-include $(SRCDIR)/common.h \
	-DREDIS_MODULE_TARGET \
	-DREDISTIMESERIES_GIT_SHA=\"$(GIT_SHA)\" \
	-DREDISMODULE_EXPERIMENTAL_API

LD_FLAGS += 
LD_LIBS += -lc -lm -L$(RMUTIL_LIBDIR) -lrmutil -L$(FAST_DOUBLE_PARSER_LIBDIR) -lfast_double_parser_c -lstdc++

ifeq ($(OS),linux)
SO_LD_FLAGS += -shared -Bsymbolic $(LD_FLAGS)
endif

ifeq ($(OS),macos)
SO_LD_FLAGS += -bundle -undefined dynamic_lookup $(LD_FLAGS)
endif

ifeq ($(PROFILE),1)
CC_FLAGS += -g -ggdb -fno-omit-frame-pointer
endif

ifeq ($(DEBUG),1)
CC_FLAGS += -g -ggdb -O0 -DDEBUG
LD_FLAGS += -g
else ifeq ($(PROFILE),1)
CC_FLAGS += -O2
else
CC_FLAGS += -O3
endif


CC_FLAGS += $(CC_FLAGS.coverage)
LD_FLAGS += $(LD_FLAGS.coverage)

_SOURCES=\
	blob.c \
	chunk.c \
	compaction.c \
	compressed_chunk.c \
	config.c \
	generic_chunk.c \
	gorilla.c \
	indexer.c \
	module.c \
	parse_policies.c \
	query_language.c \
	reply.c \
	rdb.c \
	resultset.c \
	tsdb.c \
	series_iterator.c \
	filter_iterator.c \
	fpconv.c \
	gears_integration.c \
	gears_commands.c

_TEST_SOURCES=\
	unittests.c \
	endianconv.c

_TEST_FILES=\
	unittests.c \
	unittests_parse_policies.c \
	unittests_uncompressed_chunk.c \
	unittests_compressed_chunk.c \
	unittests_parse_duplicate_policy.c

SOURCES=$(addprefix $(SRCDIR)/,$(_SOURCES))
HEADERS=$(patsubst $(SRCDIR)/%.c,$(SRCDIR)/%.h,$(SOURCES))
OBJECTS=$(patsubst $(SRCDIR)/%.c,$(BINDIR)/%.o,$(SOURCES))

TEST_SOURCES=$(addprefix $(SRCDIR)/,$(_TEST_SOURCES))
TEST_FILES=$(addprefix $(SRCDIR)/,$(_TEST_FILES))
TEST_OBJECTS=$(patsubst $(SRCDIR)/%.c,$(BINDIR)/%.o,$(TEST_SOURCES))

CC_DEPS = $(patsubst $(SRCDIR)/%.c, $(BINDIR)/%.d, $(SOURCES) $(TEST_SOURCES))

include $(MK)/defs

#----------------------------------------------------------------------------------------------

.PHONY: package tests unittests clean all install uninstall docker bindirs

all: bindirs $(TARGET)

include $(MK)/rules

rmutil:
	@echo Building $@...
	$(SHOW)$(MAKE) -C $(ROOT)/build/rmutil

$(LIBRMUTIL): rmutil

libfast_double_parser_c:
	@echo Building $@...
	$(SHOW)$(MAKE) -C $(ROOT)/src/fast_double_parser_c

$(LIBFAST_DOUBLE_PARSER): libfast_double_parser_c

docker_lint:
	$(SHOW)docker build -t llvm-toolset -f llvm.Dockerfile .
	$(SHOW)docker run --rm -w /code/src -v `pwd`/..:/code llvm-toolset make lint

lint:
	$(SHOW)clang-format -Werror -n $(SOURCES)
	$(SHOW)clang-format -Werror -n $(HEADERS)
	$(SHOW)clang-format -Werror -n $(TEST_FILES)

format:
	$(SHOW)clang-format -i $(SOURCES)
	$(SHOW)clang-format -i $(HEADERS)
	$(SHOW)clang-format -i $(TEST_FILES)

clean:
	-$(SHOW)[ -e $(BINDIR) ] && find $(BINDIR) -name '*.[oadh]' -type f -delete
	-$(SHOW)$(MAKE) -C $(ROOT)/build/rmutil clean
	-$(SHOW)$(MAKE) -C $(FAST_DOUBLE_PARSER_SRCDIR) clean
	-$(SHOW)rm -f $(TARGET) $(UNITTESTS_RUNNER)

-include $(CC_DEPS)

$(BINDIR)/%.o: $(SRCDIR)/%.c
	@echo Compiling $<...
	$(SHOW)$(CC) $(CC_FLAGS) -c $< -o $@

$(TARGET): $(BIN_DIRS) $(OBJECTS) $(LIBRMUTIL) $(LIBFAST_DOUBLE_PARSER)
	@echo Linking $@...
	$(SHOW)$(CC) $(SO_LD_FLAGS) -o $@ $(OBJECTS) $(LD_LIBS)
	$(SHOW)cd $(BINROOT)/..; ln -sf $(FULL_VARIANT)/$(notdir $(TARGET)) $(notdir $(TARGET))

#----------------------------------------------------------------------------------------------

TEST_REPORT_DIR ?= $(PWD)

ifeq ($(SIMPLE),1)
export GEN=1
export SLAVES=0
export AOF=0
export CLUSTER=0
export GEARS=0
else
export GEN ?= 1
export SLAVES ?= 1
export AOF ?= 1
export CLUSTER ?= 0
export GEARS ?= 0
endif

tests: #$(TARGET)
ifeq ($(COV),1)
	$(COVERAGE_RESET)
endif
	$(SHOW)\
		MODULE=$(realpath $(TARGET)) \
		CLUSTER=$(CLUSTER) \
		GEN=$(GEN) AOF=$(AOF) SLAVES=$(SLAVES) \
		VALGRIND=$(VALGRIND) \
		GEARS=$(GEARS) \
		$(ROOT)/tests/flow/tests.sh
ifeq ($(COV),1)
	$(COVERAGE_COLLECT_REPORT)
endif

$(UNITTESTS_RUNNER)	: $(TARGET) $(OBJECTS) $(TEST_OBJECTS)
	$(SHOW)$(CC) $(LD_FLAGS) -o $@ $(OBJECTS) $(TEST_OBJECTS) $(LD_LIBS)

unittests: $(UNITTESTS_RUNNER)
	@echo Running unit tests...
	$(SHOW)$<


BENCHMARK_ARGS = redisbench-admin run-local

ifneq ($(REMOTE),)
	BENCHMARK_ARGS = redisbench-admin run-remote 
endif

BENCHMARK_ARGS += --module_path $(realpath $(TARGET)) \
				  --required-module timeseries
ifneq ($(BENCHMARK),)
	BENCHMARK_ARGS += --test $(BENCHMARK)
endif

ifneq ($(BENCH_ARGS),)
	BENCHMARK_ARGS += $(BENCH_ARGS)
endif

benchmark: $(TARGET)
	cd ../tests/benchmarks; $(BENCHMARK_ARGS) ; cd ../../src


#----------------------------------------------------------------------------------------------

REDIS_ARGS=\
	COMPACTION_POLICY "" \
	RETNTION_POLICY 3600 \
	MAX_SAMPLE_PER_CHUNK 1024

run: $(TARGET)
	$(SHOW)redis-server --loadmodule $(realpath $(TARGET)) --dir /tmp

run_dev: $(TARGET)
	$(SHOW)redis-server --loadmodule $(realpath $(TARGET)) $(REDIS_ARGS) --dir /tmp

gdb: $(TARGET)
	$(SHOW)gdb --args `command -v redis-server` --loadmodule $(realpath $(TARGET)) --dir /tmp

cgdb: $(TARGET)
	$(SHOW)cgdb --args `command -v redis-server` --loadmodule $(realpath $(TARGET)) --dir /tmp

#----------------------------------------------------------------------------------------------

VALGRIND_ARGS=\
	--leak-check=full \
	--keep-debuginfo=yes \
	--show-reachable=no \
	--show-possibly-lost=no \
	--track-origins=yes \
	--suppressions=$(ROOT)/tests/redis_valgrind.sup \
	-v redis-server

valgrind: $(TARGET)
	$(SHOW)valgrind $(VALGRIND_ARGS) --loadmodule $(realpath $(TARGET)) $(REDIS_ARGS) --dir /tmp

CALLGRIND_ARGS=\
	--tool=callgrind \
	--dump-instr=yes \
	--simulate-cache=no \
	--collect-jumps=yes \
	--collect-atstart=yes \
	--instr-atstart=yes \
	-v redis-server --protected-mode no --save "" --appendonly no

callgrind: $(TARGET)
	$(SHOW)valgrind $(CALLGRIND_ARGS) --loadmodule $(realpath $(TARGET)) $(REDIS_ARGS) --dir /tmp

#----------------------------------------------------------------------------------------------

install: all
	$(SHOW)mkdir -p $(INSTALL_LIB)
	$(SHOW)$(INSTALL) $(TARGET) $(INSTALL_LIB)

uninstall:
	$(SHOW)rm -f $(INSTALL_LIB)/$(notdir $(TARGET))

docker:
	$(SHOW)cd .. && docker build -t redis-tsdb .

coverage:
	$(SHOW)DEBUG=1 COVERAGE=1 make tests
	$(SHOW)DEBUG=1 COVERAGE=1 make unittests
	$(SHOW)mkdir -p tmp/lcov
	$(SHOW)lcov -d ../bin/linux-x64-debug/src -c -o tmp/lcov/hiredis.info
	$(SHOW)lcov -l tmp/lcov/hiredis.info
	$(SHOW)genhtml --legend -o tmp/lcov/report tmp/lcov/hiredis.info > /dev/null 2>&1

#----------------------------------------------------------------------------------------------

package: $(TARGET)
	@echo Creating packages...
	$(SHOW)$(ROOT)/sbin/pack.sh
