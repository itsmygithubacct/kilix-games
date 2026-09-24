# Every game builds and tests on its own; this runs them all in sequence.
GAMES := solitaire-tui kilix-lander joustix kilix-brokeout bashed-earth \
	kilix-lights kilix-jpak kilix-pong chess-bash kilix-rancher kilix-fishtank \
	tictactoe-tui

.PHONY: test all clean $(GAMES)

test: $(GAMES)

$(GAMES):
	$(MAKE) -C $@ test

all:
	@for g in $(filter-out solitaire-tui,$(GAMES)); do $(MAKE) -C $$g all || exit 1; done

clean:
	@for g in $(GAMES); do $(MAKE) -C $$g clean || exit 1; done
