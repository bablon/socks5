CC = gcc
CFLAGS = -g -Wall -Werror
LDFLAGS =

bins = s5
objs = $(bins:=.o)

# cmd_cc_o_c = $(CC) $(CFLAGS) -c -o $@ $<
# quiet_cmd_cc_o_c = CC      $@

ifneq ($(V),1)
	V=0
else
	V=1
endif

V_CC_0 = @echo "    CC     " $@;
V_CC_1 = 
V_CC = $(V_CC_$(V))

V_LN_0 = @echo "    LINK   " $@;
V_LN_1 = 
V_LN = $(V_LN_$(V))

all : $(bins)

$(bins) : % : %.o
	$(V_LN)$(CC) -o $@ $< $(LDFLAGS)

$(objs) : %.o : %.c
	$(V_CC)$(CC) $(CFLAGS) -c -o $@ $<
	@$(CC) -MM $(CFLAGS) $*.c > $*.d

.PHONY: clean

clean:
	$(RM) $(bins) $(objs)
	$(RM) *.d
