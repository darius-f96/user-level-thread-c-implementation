CXX := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -Iinclude

LIB := libult.a
OBJS := src/ult.o

all: $(LIB) example mutex_demo

$(LIB): $(OBJS)
	ar rcs $@ $^

src/ult.o: src/ult.cpp include/ult.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

example: examples/demo.o $(LIB)
	$(CXX) $(CXXFLAGS) -o $@ examples/demo.o $(LIB)

examples/demo.o: examples/demo.cpp include/ult.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

mutex_demo: examples/mutex_demo.o $(LIB)
	$(CXX) $(CXXFLAGS) -o $@ examples/mutex_demo.o $(LIB)

examples/mutex_demo.o: examples/mutex_demo.cpp include/ult.h
	$(CXX) $(CXXFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) examples/demo.o examples/mutex_demo.o $(LIB) example mutex_demo

.PHONY: all clean
