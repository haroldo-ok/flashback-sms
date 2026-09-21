# Headless Flashback extractor (links REminiscence, no SDL needed)
RS   := reminiscence
SRCS := collision.cpp cpc_player.cpp cutscene.cpp decode_mac.cpp file.cpp fs.cpp game.cpp \
        graphics.cpp menu.cpp midi_parser.cpp mixer.cpp mod_player.cpp piege.cpp \
        prf_player.cpp protection.cpp resource.cpp resource_aba.cpp resource_mac.cpp \
        resource_paq.cpp scaler.cpp screenshot.cpp seq_player.cpp sfx_player.cpp staticres.cpp \
        unpack.cpp util.cpp video.cpp
OBJS := $(addprefix obj/,$(SRCS:.cpp=.o)) obj/fbdump.o obj/ogg_stub.o
CXXFLAGS := -O2 -w -I$(RS) -DUSE_ZLIB
fbdump: $(OBJS)
	$(CXX) -o $@ $(OBJS) -lz
obj/%.o: $(RS)/%.cpp
	@mkdir -p obj; $(CXX) $(CXXFLAGS) -c $< -o $@
obj/fbdump.o: fbdump.cpp
	@mkdir -p obj; $(CXX) $(CXXFLAGS) -c $< -o $@
clean:
	rm -rf obj fbdump
obj/ogg_stub.o: ogg_stub.cpp
	@mkdir -p obj; $(CXX) $(CXXFLAGS) -c $< -o $@
