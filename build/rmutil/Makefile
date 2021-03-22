
ROOT=../..
MK.pyver:=3

include $(ROOT)/deps/readies/mk/main

MK_CUSTOM_CLEAN=1

BINDIR=$(BINROOT)/rmutil
SRCDIR=$(ROOT)/deps/RedisModulesSDK/rmutil

CC=gcc

CC_FLAGS=\
	-I$(SRCDIR)/.. \
	-DREDIS_MODULE_TARGET \
	-Wall \
	-fPIC \
	-Wno-unused-function \
	-std=gnu99 \
	-MMD -MF $(@:.o=.d)

ifeq ($(PROFILE),1)
CC_FLAGS += -g -ggdb -fno-omit-frame-pointer
endif

ifeq ($(DEBUG),1)
CC_FLAGS += -g -ggdb -O0
else
ifeq ($(PROFILE),1)
CC_FLAGS += -O2
else
CC_FLAGS += -O3
endif
endif

_SOURCES=util.c strings.c sds.c vector.c alloc.c periodic.c

SOURCES=$(addprefix $(SRCDIR)/,$(_SOURCES))
OBJECTS=$(patsubst $(SRCDIR)/%.c,$(BINDIR)/%.o,$(SOURCES))

CC_DEPS = $(patsubst $(SRCDIR)/%.c, $(BINDIR)/%.d, $(SOURCES))

TARGET=$(BINDIR)/librmutil.a

include $(MK)/defs

#----------------------------------------------------------------------------------------------

.PHONY: all clean test_vector test_periodic test

all: bindirs $(TARGET)

include $(MK)/rules

clean:
	-$(SHOW)[ -e $(BINDIR) ] && find $(BINDIR) -name '*.[oadh]' -type f -delete

-include $(CC_DEPS)

$(BINDIR)/%.o: $(SRCDIR)/%.c
	@echo Compiling $<...
	$(SHOW)$(CC) $(CC_FLAGS) -c $< -o $@

$(TARGET): $(OBJECTS)
	@echo Creating $@...
	$(SHOW)ar rcs $@ $^

#----------------------------------------------------------------------------------------------

$(BINDIR)/test_vector: test_vector.o vector.o
	$(SHOW)$(CC) -Wall -o $@ $^ -lc -lpthread -O0
	$(SHOW)sh -c ./$@

$(BINDIR)/test_periodic: test_periodic.o periodic.o
	$(SHOW)$(CC) -Wall -o $@ $^ -lc -lpthread -O0
	$(SHOW)sh -c ./$@
	
test: test_periodic test_vector
