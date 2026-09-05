#-------------------------------------------------------------------------------
.SUFFIXES:
#-------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITPRO)),)
$(error "Please set DEVKITPRO in your environment. export DEVKITPRO=<path to>/devkitpro")
endif

TOPDIR ?= $(CURDIR)

#-------------------------------------------------------------------------------
# APP_NAME sets the long name of the application
# APP_SHORTNAME sets the short name of the application
# APP_AUTHOR sets the author of the application
#-------------------------------------------------------------------------------
APP_NAME	:= Moonlight
APP_SHORTNAME	:= Moonlight
APP_AUTHOR	:= GaryOderNichts

include $(DEVKITPPC)/wii_rules

#-------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
#-------------------------------------------------------------------------------
TARGET		:=	moonlight
BUILD		:=	build
SOURCES		:=	src \
				src/wii \
				libgamestream \
				third_party/moonlight-common-c/src \
				third_party/moonlight-common-c/reedsolomon \
			third_party/moonlight-common-c/enet \
			third_party/h264bitstream \
			third_party/uuidstr
DATA		:=
INCLUDES	:=	src/wii \
				libgamestream \
				third_party/moonlight-common-c/src \
				third_party/moonlight-common-c/reedsolomon \
			third_party/moonlight-common-c/enet/include \
			third_party/h264bitstream \
			third_party/uuidstr \
			third_party/ffmpeg-wii/include
SOURCE_FILES	:=

#-------------------------------------------------------------------------------
# options for code generation
#-------------------------------------------------------------------------------
CFLAGS	:=	-O3 -ffunction-sections -fdata-sections \
			$(MACHDEP) -mmultiple -msdata \
			-ffast-math -frename-registers \
			-D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64

CFLAGS	+=	$(INCLUDE) -D__WII__ -DUSE_MBEDTLS -DHAS_SOCKLEN_T -DDEBUG

CXXFLAGS	:= $(CFLAGS)

ASFLAGS	:=	$(ARCH)
LDFLAGS	=	$(ARCH) -T$(DEVKITPPC)/powerpc-eabi/lib/rvl.ld -Wl,-Map,$(notdir $*.map)

 LIBS	:=	-lwiiuse -lbte -lfat -logc -lopus \
			-lmbedtls -lmbedx509 -lmbedcrypto -lexpat \
			-lfreetype -lbrotlidec -lbrotlicommon -lpng -lz -lbz2 \
			-L$(TOPDIR)/third_party/ffmpeg-wii/lib -lavcodec -lswscale -lavutil \
			-lm

.DEFAULT_GOAL := all

# ffmpeg: no prebuilt libs in the repo, build the static libs at runtime
# from the committed source tree (needs DEVKITPRO + DEVKITPPC)
FFMPEG_SRC	:=	$(TOPDIR)/third_party/ffmpeg-wii/src
FFMPEG_LIBS	:=	$(TOPDIR)/third_party/ffmpeg-wii/lib/libavcodec.a \
		$(TOPDIR)/third_party/ffmpeg-wii/lib/libswscale.a \
		$(TOPDIR)/third_party/ffmpeg-wii/lib/libavutil.a

FFMPEG_SRCS	:=	$(shell find $(FFMPEG_SRC) \( -name '*.c' -o -name '*.h' \
		-o -name '*.S' -o -name '*.mak' -o -name 'Makefile' \) 2>/dev/null)

$(FFMPEG_LIBS): $(FFMPEG_SRCS)
	@echo building ffmpeg ...
	@cd $(FFMPEG_SRC) && revision=0.10 $(MAKE) --no-print-directory \
		libavutil/libavutil.a libavcodec/libavcodec.a libswscale/libswscale.a
	@mkdir -p $(TOPDIR)/third_party/ffmpeg-wii/lib
	@cp $(FFMPEG_SRC)/libavutil/libavutil.a \
	    $(FFMPEG_SRC)/libavcodec/libavcodec.a \
	    $(FFMPEG_SRC)/libswscale/libswscale.a $(TOPDIR)/third_party/ffmpeg-wii/lib/

#-------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level
# containing include and lib
#-------------------------------------------------------------------------------
LIBDIRS	:= $(PORTLIBS) $(LIBOGC_LIB)

#-------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#-------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#-------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir)) \
			$(foreach sf,$(SOURCE_FILES),$(CURDIR)/$(dir $(sf)))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c))) \
			$(foreach f,$(SOURCE_FILES),$(notdir $(f)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#-------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#-------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#-------------------------------------------------------------------------------
	export LD	:=	$(CC)
#-------------------------------------------------------------------------------
else
#-------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#-------------------------------------------------------------------------------
endif
#-------------------------------------------------------------------------------

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES))
export OFILES_SRC	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES 	:=	$(OFILES_BIN) $(OFILES_SRC)
export HFILES_BIN	:=	$(addsuffix .h,$(subst .,_,$(BINFILES)))

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(LIBOGC_INC) \
			-I$(CURDIR)/$(BUILD) -I$(DEVKITPRO)/portlibs/ppc/include/freetype2

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib) -L$(LIBOGC_LIB)

.PHONY: $(BUILD) clean all dist

#-------------------------------------------------------------------------------
all: $(BUILD)

#TODO: fix this to make this generate at least the XML dynamically
dist: all
	mkdir -p dist/wii/apps/moonlight
	cp moonlight.dol dist/wii/apps/moonlight/boot.dol
	cp icon.png dist/wii/apps/moonlight/
	cp meta.xml dist/wii/apps/moonlight/

# font_data.c is committed (the devkitPPC image has no xxd); only
# regenerate when font.ttf changes and xxd is available.
src/wii/font_data.c: src/wii/font.ttf
	@if command -v xxd >/dev/null 2>&1; then \
		cd src/wii && xxd -i font.ttf > font_data.c; \
	else \
		echo "xxd not found; using committed font_data.c"; \
	fi

$(BUILD): src/wii/font_data.c
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

#-------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).dol $(TARGET).elf \
		third_party/ffmpeg-wii/lib/*.a \
		$(FFMPEG_SRC)/*.o $(FFMPEG_SRC)/*/*.o $(FFMPEG_SRC)/*/*/*.o \
		$(FFMPEG_SRC)/version.h $(FFMPEG_SRC)/.version

#-------------------------------------------------------------------------------
else
.PHONY:	all

DEPENDS	:=	$(OFILES:.o=.d)

#-------------------------------------------------------------------------------
# main targets
#-------------------------------------------------------------------------------
all	:	$(OUTPUT).dol

$(OUTPUT).dol	:	$(OUTPUT).elf
$(OUTPUT).elf	:	$(OFILES) $(FFMPEG_LIBS)

$(OFILES_SRC)	: $(HFILES_BIN)

#-------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#-------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#-------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

-include $(DEPENDS)

#-------------------------------------------------------------------------------
endif
#-------------------------------------------------------------------------------
