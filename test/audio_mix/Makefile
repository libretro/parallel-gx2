# The core's audio arithmetic between AX and the frontend (src/audio/AudioMix.h)
# (run_tests.sh). SANITIZER=address,undefined for the sanitizer run.
SRC := ../../src
CXX ?= g++
FLAGS := -O1 -g -I$(SRC)
ifneq ($(SANITIZER),)
   FLAGS += -fsanitize=$(SANITIZER) -fno-omit-frame-pointer
endif
ifneq (,$(findstring mingw,$(CXX)))
   LIBS += -static
   EXE := .exe
endif

audio_mix_test$(EXE): audio_mix_test.cpp $(SRC)/audio/AudioMix.h
	$(CXX) $(FLAGS) -std=gnu++20 audio_mix_test.cpp $(LIBS) -o $@

clean:
	rm -f audio_mix_test audio_mix_test.exe
