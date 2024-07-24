# GNU Make workspace makefile autogenerated by Premake

.NOTPARALLEL:

ifndef config
  config=debug_x64
endif

ifndef verbose
  SILENT = @
endif

ifeq ($(config),debug_x64)
  ServerStorage_config = debug_x64
endif
ifeq ($(config),release_x64)
  ServerStorage_config = release_x64
endif

PROJECTS := ServerStorage

.PHONY: all clean help $(PROJECTS) 

all: $(PROJECTS)

ServerStorage:
ifneq (,$(ServerStorage_config))
	@echo "==== Building ServerStorage ($(ServerStorage_config)) ===="
	@${MAKE} --no-print-directory -C . -f ServerStorage.make config=$(ServerStorage_config)
endif

clean:
	@${MAKE} --no-print-directory -C . -f ServerStorage.make clean

help:
	@echo "Usage: make [config=name] [target]"
	@echo ""
	@echo "CONFIGURATIONS:"
	@echo "  debug_x64"
	@echo "  release_x64"
	@echo ""
	@echo "TARGETS:"
	@echo "   all (default)"
	@echo "   clean"
	@echo "   ServerStorage"
	@echo ""
	@echo "For more information, see https://github.com/premake/premake-core/wiki"