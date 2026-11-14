# Include after MOCE_SHEILD is set by the application Makefile.
ifneq ($(MOCE_SHEILD),none)
$(error This Board currently accepts only explicit MOCE_SHEILD=none)
endif
CFLAGS += -DMOCE_SHEILD_NONE=1
CPPFLAGS += -DMOCE_SHEILD_NONE=1
