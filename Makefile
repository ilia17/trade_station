CXX      = g++
CXXFLAGS = -std=c++17 -O2 -pthread -I. \
           -Iimgui \
           $(shell sdl2-config --cflags)

LDFLAGS  = -lssl -lcrypto -lboost_system -lpthread -lprotobuf -lz \
           $(shell sdl2-config --libs) \
           -lGL -lboost_thread

TARGET = orderbook_stream

PROTO_SRCS  = $(wildcard proto/*.pb.cc)

IMGUI_SRCS  = imgui/imgui.cpp \
              imgui/imgui_draw.cpp \
              imgui/imgui_tables.cpp \
              imgui/imgui_widgets.cpp \
              imgui/backends/imgui_impl_sdl2.cpp \
              imgui/backends/imgui_impl_opengl3.cpp

TRADING_SRCS = mexc/mexc_trader.cpp \
               mexc/mexc_order_stream.cpp \
               gate/gate_trader.cpp \
               bingx/bingx_trader.cpp \
               bingx/bingx_order_stream.cpp \
               lbank/lbank_trader.cpp

SRCS = main.cpp \
       mexc/mexc_handler.cpp \
       gate/gate_handler.cpp \
       bingx/bingx_handler.cpp \
       lbank/lbank_handler.cpp \
       ui/render.cpp \
       $(TRADING_SRCS) \
       $(IMGUI_SRCS) \
       $(PROTO_SRCS)

$(TARGET): $(SRCS)
	$(CXX) $(CXXFLAGS) $(SRCS) $(LDFLAGS) -o $(TARGET)

clean:
	rm -f $(TARGET)
