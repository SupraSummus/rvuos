# The fault tracer's decoder, tested on the host, as keys.c is, since it fetches nothing.
TRACE_HOST := build/host/tracer

$(TRACE_HOST)/trace.o: user/tracer/trace.c user/tracer/trace.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow $(HOST_SAN) -Iuser/tracer -c $< -o $@

$(TRACE_HOST)/decode-test.o: user/tracer/test/decode-test.c user/tracer/trace.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow $(HOST_SAN) -Iuser/tracer -c $< -o $@

$(TRACE_HOST)/decode-test: $(TRACE_HOST)/trace.o $(TRACE_HOST)/decode-test.o
	$(HOST_CC) $(HOST_SAN) $^ -o $@

.PHONY: decode-test
decode-test: $(TRACE_HOST)/decode-test
	$(TRACE_HOST)/decode-test
