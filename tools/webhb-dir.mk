# Where webhb is - for the Makefile and for harness/Makefile, which sets REPO.
# In this order: WEBHB_DIR when given, the release installed in ext/webhb, a
# checkout next to this repository, and otherwise the release webhb.lock pins
# is installed now.
REPO ?= .
ifndef WEBHB_DIR
  ifneq ($(wildcard $(REPO)/ext/webhb/VERSION),)
    WEBHB_DIR := $(REPO)/ext/webhb
  else ifneq ($(wildcard $(REPO)/../webhb/webhb/webhb.mk),)
    WEBHB_DIR := $(REPO)/../webhb
    $(info webhb: using the checkout $(abspath $(WEBHB_DIR)) - no release is installed in ext/webhb)
  else
    WEBHB_DIR := $(REPO)/ext/webhb
    $(info webhb: installing the release webhb.lock names)
    $(shell cd $(REPO) && tools/get-webhb.sh webhb.lock ext/webhb >&2)
  endif
endif
