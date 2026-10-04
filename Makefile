PREFIX ?= $(HOME)/.local/bin
RC     ?= $(HOME)/.bashrc
OWNER  ?= noxthedevwindev-greatest
REPO   ?= simpledir
ASSETS ?= sd install.sh

.PHONY: install uninstall test assets release

install: binaries
	@if grep -q '>>> simpledir >>>' $(RC); then \
		echo "$(RC) already patched, left alone"; \
	else \
		echo "" >> $(RC); \
		echo "# >>> simpledir >>>" >> $(RC); \
		$(PREFIX)/sdcfg init >> $(RC); \
		echo "# <<< simpledir <<<" >> $(RC); \
		echo "wired up $(RC)"; \
	fi

# one file, two names. the program decides what it is by argv[0], so the
# second name is only ever a symlink.
.PHONY: binaries
binaries:
	install -Dm755 sd $(PREFIX)/sd
	ln -sfn sd $(PREFIX)/sdcfg

uninstall:
	rm -f $(PREFIX)/sd $(PREFIX)/sdcfg
	cp -p $(RC) $(RC).bak.$(shell date +%Y%m%d%H%M%S)
	sed -i '/# >>> simpledir >>>/,/# <<< simpledir <<</d' $(RC)
	@echo "removed binaries + rc block (config kept in ~/.simpledir)"

# the release assets: the tool itself and the installer, nothing compiled.
# GitHub drops the executable bit on download, so `sdcfg update` and install.sh
# both chmod what they fetch.
assets:
	@mkdir -p dist
	@for f in $(ASSETS); do install -m 755 "$$f" "dist/$$f"; done
	@ls -l dist

# tag, push, publish, upload the assets
release: test assets
	@version=$$(sed -n 's/^VERSION = "\(.*\)"/\1/p' sd); \
	echo "releasing v$$version"; \
	test -n "$$version" || { echo "could not read VERSION"; exit 1; }; \
	git tag -a "v$$version" -m "$$version"; \
	git push origin "v$$version"; \
	gh release create "v$$version" --title "v$$version" --generate-notes $(foreach f,$(ASSETS),"dist/$(f)#$(f)")

test:
	bash tests/run.sh
