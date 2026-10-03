PREFIX ?= $(HOME)/.local/bin
RC     ?= $(HOME)/.bashrc
OWNER  ?= noxthedevwindev-greatest
REPO   ?= simpledir
ASSETS ?= simpledir install.sh

.PHONY: install uninstall test assets release

install:
	install -Dm755 simpledir $(PREFIX)/simpledir
	@if grep -q '>>> simpledir >>>' $(RC); then \
		echo "$(RC) already patched, left alone"; \
	else \
		echo "" >> $(RC); \
		echo "# >>> simpledir >>>" >> $(RC); \
		$(PREFIX)/simpledir init >> $(RC); \
		echo "# <<< simpledir <<<" >> $(RC); \
		echo "wired up $(RC)"; \
	fi

uninstall:
	rm -f $(PREFIX)/simpledir
	cp -p $(RC) $(RC).bak.$(shell date +%Y%m%d%H%M%S)
	sed -i '/# >>> simpledir >>>/,/# <<< simpledir <<</d' $(RC)
	@echo "removed binary + rc block (config kept in ~/.simpledir)"

# the release assets: the tool itself and the installer, nothing compiled.
# GitHub drops the executable bit on download, so `simpledir update` and
# install.sh both chmod what they fetch.
assets:
	@mkdir -p dist
	@for f in $(ASSETS); do install -m 755 "$$f" "dist/$$f"; done
	@ls -l dist

# tag, push, publish, upload the assets
release: test assets
	@version=$$(sed -n 's/^VERSION = "\(.*\)"/\1/p' simpledir); \
	echo "releasing v$$version"; \
	test -n "$$version" || { echo "could not read VERSION"; exit 1; }; \
	git tag -a "v$$version" -m "$$version"; \
	git push origin "v$$version"; \
	gh release create "v$$version" --title "v$$version" --generate-notes $(foreach f,$(ASSETS),"dist/$(f)#$(f)")

test:
	bash tests/run.sh
