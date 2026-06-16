TARGET=sample_vi_od

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

ifneq (,$(TDL_LIB))
LDFLAGS += -Wl,-rpath-link,$(TDL_LIB)
endif
ifneq (,$(SYS_LIB))
LDFLAGS += -Wl,-rpath-link,$(SYS_LIB)
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
CXXFLAGS += -I..

LDFLAGS += -lini -lsns_full -lsample -lisp -lvdec -lvenc -lawb -lae -laf -lcvi_bin -lcvi_bin_isp -lmisc -lisp_algo -lsys -lvi -lvo -lvpss -lrgn -lgdc
LDFLAGS += -ltdl_core
LDFLAGS += -lopencv_core -lopencv_imgproc -lopencv_imgcodecs
LDFLAGS += -lcvikernel -lcvimath -lcviruntime
LDFLAGS += -lcvi_rtsp
LDFLAGS += $(THREAD_FLAGS) -latomic

C_SOURCE = $(wildcard *.c)
C_OBJS = $(patsubst %.c,%.o,$(C_SOURCE))
CVI_MEDIA_DIR = ../cvi_media
CVI_MEDIA_SOURCE = $(CVI_MEDIA_DIR)/media_pipeline.cpp \
	$(CVI_MEDIA_DIR)/tdl_pipeline.cpp \
	$(CVI_MEDIA_DIR)/tdl_draw.cpp
CXX_SOURCE = $(wildcard *.cpp)
CXX_OBJS = $(patsubst %.cpp,%.o,$(CXX_SOURCE))
CVI_MEDIA_OBJS = $(patsubst $(CVI_MEDIA_DIR)/%.cpp,cvi_media_%.o,$(CVI_MEDIA_SOURCE))
OBJS = $(C_OBJS) $(CXX_OBJS) $(CVI_MEDIA_OBJS)

$(TARGET): $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -o $@ -c $<

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -o $@ -c $<

cvi_media_%.o: $(CVI_MEDIA_DIR)/%.cpp
	$(CXX) $(CXXFLAGS) -o $@ -c $<

.PHONY: clean
clean:
	@rm -rf *.o
	@rm -rf $(OBJS)
	@rm -rf $(TARGET)
