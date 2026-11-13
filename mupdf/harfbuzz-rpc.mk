# HarfBuzz 1.9.0 for the RiscPC build (mupdf/build.sh riscpc): its sources
# from its own Makefile.sources, into OUT/libfzhb.a.
include Makefile.sources
SRCS := $(filter %.cc,$(HB_BASE_sources) $(HB_FALLBACK_sources) $(HB_OT_sources) $(HB_FT_sources) $(HB_UCDN_sources))
OBJS := $(SRCS:%.cc=$(OUT)/%.o) $(OUT)/ucdn.o
all: $(OUT)/libfzhb.a
$(OUT)/%.o: %.cc
	@mkdir -p $(OUT)
	$(CXX) $(FLAGS) -fno-exceptions -fno-rtti -fno-threadsafe-statics -c $< -o $@
$(OUT)/ucdn.o: hb-ucdn/ucdn.c
	$(CC) $(FLAGS) -c $< -o $@
$(OUT)/libfzhb.a: $(OBJS)
	rm -f $@; $(AR) rcs $@ $^
