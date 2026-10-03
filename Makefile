PREFIX ?= $(HOME)/.local/bin
RC     ?= $(HOME)/.bashrc

.PHONY: install uninstall test

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

test:
	bash tests/run.sh
