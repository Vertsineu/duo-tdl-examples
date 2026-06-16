TARGET=sample_vi_rtsp

ifeq (,$(COMMON_DIR))
$(error COMMON_DIR is not set)
endif

ifeq (,$(TOOLCHAIN_PREFIX))
$(error TOOLCHAIN_PREFIX is not set)
endif

ifeq ($(origin CXX), default)
CXX = $(TOOLCHAIN_PREFIX)g++
endif

ifeq (,$(CFLAGS))
$(error CFLAGS is not set)
endif

ifeq (,$(LDFLAGS))
$(error LDFLAGS is not set)
endif

ifeq ($(CHIP), CV181X)
CFLAGS += -DCV181X -D__CV181X__
else ifeq ($(CHIP), CV180X)
CFLAGS += -DCV180X -D__CV180X__
else
$(error CHIP is not set)
endif

SAMPLE_FLAGS = -Wno-pointer-to-int-cast -fsigned-char -Wno-format-truncation -fdiagnostics-color=always -s
THREAD_FLAGS = -pthread

CFLAGS += -std=gnu11 $(SAMPLE_FLAGS) $(THREAD_FLAGS)
CXXFLAGS += $(filter-out -std=gnu11 -Wno-pointer-to-int-cast,$(CFLAGS)) -std=gnu++11

LDFLAGS += -lini -lsns_full -lsample -lisp -lvdec -lvenc -lawb -lae -laf -lcvi_bin -lcvi_bin_isp -lmisc -lisp_algo -lsys -lvi -lvo -lvpss -lrgn -lgdc
LDFLAGS += -lcvi_rtsp
LDFLAGS += $(THREAD_FLAGS) -latomic

COMMON_SRC = $(COMMON_DIR)/middleware_utils.c
COMMON_OBJ = $(COMMON_SRC:.c=.o)

C_SOURCE = $(wildcard *.c)
C_OBJS = $(patsubst %.c,%.o,$(C_SOURCE))
CXX_SOURCE = $(wildcard *.cpp)
CXX_OBJS = $(patsubst %.cpp,%.o,$(CXX_SOURCE))
OBJS = $(C_OBJS) $(CXX_OBJS)

$(TARGET): $(OBJS) $(COMMON_OBJ)
	$(CXX) $(CXXFLAGS) -o $@ $(COMMON_OBJ) $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -o $@ -c $<

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -o $@ -c $<

.PHONY: clean
clean:
	@rm -rf *.o
	@rm -rf $(COMMON_OBJ)
	@rm -rf $(OBJS)
	@rm -rf $(TARGET)
