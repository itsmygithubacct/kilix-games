GAMES := solitaire-tui

.PHONY: test $(GAMES)

test: $(GAMES)

$(GAMES):
	$(MAKE) -C $@ test
