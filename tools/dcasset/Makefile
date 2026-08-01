# tools/dcasset -- GameCube ISO -> Dreamcast disc layout
#
# There is nothing to compile: dcasset is pure Python 3.9+ stdlib. This
# Makefile exists so the pipeline has one documented entry point, and so the
# ISO path / output path are set in exactly one place.
#
#   make report                 # padding-vs-content measurement
#   make report-json            # + machine-readable dump
#   make extract                # write the Dreamcast disc tree
#   make verify                 # re-hash the tree against manifest.json
#   make relmap                 # which REL/DOL bytes pc_assets.c really needs
#   make all                    # report + extract + verify
#
# Override anything on the command line:
#   make extract ISO=/path/to/other.gcm OUT=/tmp/other-root

PYTHON ?= python3
HERE   := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
REPO   := $(abspath $(HERE)/../..)

# The user's own GameCube image. READ ONLY. Never copied into the repo.
ISO ?= /Users/gabe/Documents/GitHub/OpenCrossing-Anbernic/harness/rom/Animal Crossing.iso

# Output MUST live outside the repository -- dcasset.py refuses otherwise.
OUT ?= /tmp/opencrossing-dc/discroot
REPORT_JSON ?= /tmp/opencrossing-dc/report.json
RELMAP_JSON ?= /tmp/opencrossing-dc/relmap.json

DCASSET := $(PYTHON) $(HERE)/dcasset.py

.PHONY: all report report-json extract extract-szs verify relmap lint clean help

all: report extract verify

help:
	@sed -n '2,20p' $(HERE)/Makefile

report:
	$(DCASSET) report "$(ISO)"

report-json:
	@mkdir -p $(dir $(REPORT_JSON))
	$(DCASSET) report "$(ISO)" --json "$(REPORT_JSON)"

extract:
	$(DCASSET) extract "$(ISO)" --out "$(OUT)"

# Variant that leaves foresta.rel Yaz0-compressed on the disc (smaller disc,
# but the Dreamcast then needs 15.6 MB of RAM to expand it -- do not use
# unless a streaming Yaz0 reader exists).
extract-szs:
	$(DCASSET) extract "$(ISO)" --out "$(OUT)" --keep-szs

verify:
	$(DCASSET) verify "$(OUT)"

relmap:
	@mkdir -p $(dir $(RELMAP_JSON))
	$(DCASSET) relmap --repo "$(REPO)" --json "$(RELMAP_JSON)" --spans

lint:
	$(PYTHON) -m flake8 $(HERE)

# Only ever removes the (out-of-repo) extraction directory.
clean:
	rm -rf "$(OUT)" "$(REPORT_JSON)" "$(RELMAP_JSON)"
