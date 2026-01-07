CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -Iinclude

LIB := libult.a
OBJS := src/ult.o

all: $(LIB) example

$(LIB): $(OBJS)
	ar rcs $@ $^

src/ult.o: src/ult.cpp include/ult.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

example: examples/demo.o $(LIB)
	$(CXX) $(CXXFLAGS) -o $@ examples/demo.o $(LIB)

examples/demo.o: examples/demo.cpp include/ult.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) examples/demo.o $(LIB) example

.PHONY: all clean
