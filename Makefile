OUT=build
OBJ=$(OUT)\obj
UNPACKER=src\unpacker
MH=$(UNPACKER)\Mhook\minhook

CFLAGS=/nologo /O2 /MT /W3
INCLUDES=/I "$(UNPACKER)\Mhook" /I "$(MH)\include" /I "$(MH)\src"

UNPACKER_SOURCES= \
    $(UNPACKER)\AllodsUnpacker14.cpp \
    $(UNPACKER)\Containers\Fixups.cpp \
    $(UNPACKER)\Containers\HrefMap.cpp \
    $(UNPACKER)\Containers\PackReader.cpp \
    $(UNPACKER)\Containers\SlotMap.cpp \
    $(UNPACKER)\Containers\Types.cpp \
    $(UNPACKER)\Mhook\Hooks.cpp \
    $(UNPACKER)\Tools\Freeze.cpp \
    $(UNPACKER)\Tools\Fs.cpp \
    $(UNPACKER)\Tools\Log.cpp \
    $(UNPACKER)\Unpack\EngineWriter.cpp \
    $(UNPACKER)\Unpack\HrefWriter.cpp \
    $(UNPACKER)\Unpack\MapLoader.cpp \
    $(UNPACKER)\Unpack\Sink.cpp

MINHOOK_SOURCES= \
    $(MH)\src\hook.c \
    $(MH)\src\buffer.c \
    $(MH)\src\trampoline.c \
    $(MH)\src\hde\hde32.c

all: $(OUT)\pango.dll $(OUT)\AllodsUnpacker14.dll $(OUT)\AllodsUnpacker14.ini

$(OUT)\pango.dll: src\carrier\pango_carrier.c src\carrier\pango_exports.h
	@if not exist "$(OUT)" mkdir "$(OUT)"
	@if not exist "$(OBJ)" mkdir "$(OBJ)"
	cl $(CFLAGS) /LD /Fo$(OBJ)\ src\carrier\pango_carrier.c \
	    /Fe:$@ /link /IMPLIB:$(OUT)\pango.lib kernel32.lib

$(OUT)\AllodsUnpacker14.dll: $(UNPACKER_SOURCES) $(MINHOOK_SOURCES) $(UNPACKER)\Header.h $(UNPACKER)\Offsets.h
	@if not exist "$(OUT)" mkdir "$(OUT)"
	@if not exist "$(OBJ)" mkdir "$(OBJ)"
	cl $(CFLAGS) /LD /EHsc /DWIN32 $(INCLUDES) \
	    $(UNPACKER_SOURCES) $(MINHOOK_SOURCES) /Fo$(OBJ)\ \
	    /Fe:$@ /link /IMPLIB:$(OUT)\AllodsUnpacker14.lib kernel32.lib user32.lib

$(OUT)\AllodsUnpacker14.ini: config\AllodsUnpacker14.ini
	@if not exist "$(OUT)" mkdir "$(OUT)"
	@copy /y $** $@ >nul

clean:
	@if exist "$(OUT)" rmdir /s /q "$(OUT)"

rebuild: clean all
