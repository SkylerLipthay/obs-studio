project(duktape)

set(duktape_HEADERS
	duktape/duktape.h)
set(duktape_SOURCES
	duktape/duktape.c)

add_library(duktape STATIC
	${duktape_SOURCES}
	${duktape_HEADERS})
target_include_directories(duktape
	PUBLIC .)
target_link_libraries(duktape)
