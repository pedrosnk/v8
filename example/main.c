#include <stdlib.h>
#include <stdio.h>

#include <v8/v8.h>


void handler(const V8Context * context, const V8Request * request,
						 V8Response * response);

static V8Action actions[] = {
	{V8_SIMPLE_ACTION, V8_GET, "/", handler},
	{V8_NO_ACTION, V8_UNKNOWN, "", NULL}
};

void handler(const V8Context * context, const V8Request * request,
						 V8Response * response)
{
	printf("teste: %s\n", v8_request_param(request, "teste"));
	printf("teste2: %s\n", v8_request_param(request, "teste2"));
	printf("teste3: %s\n", v8_request_param(request, "teste3"));
}

int main(int argc, char * argv[])
{
	V8 * v8;

	v8 = v8_init(NULL, actions);

	v8_start(v8);

	return 0;
}
