PYTHON ?= python3

.PHONY: all check-policy test run simulate clean

# Nothing to compile (pure Python): `all` checks that the launcher can run
# and that the shipped network is the one its manifest describes, so the
# catalog's `make -C solitaire-tui all` is a real gate.
all: check-policy
	@test -x bin/solitaire-tui
	@PYTHONDONTWRITEBYTECODE=1 ./bin/solitaire-tui --version

check-policy:
	@PYTHONDONTWRITEBYTECODE=1 $(PYTHON) tools/neural/check-policy.py

test: check-policy
	PYTHONDONTWRITEBYTECODE=1 $(PYTHON) -m unittest discover -s tests -t .

run:
	./bin/solitaire-tui

simulate:
	./bin/solitaire-tui --simulate greedy --games 200

clean:
	find . -name __pycache__ -type d -prune -exec rm -rf {} +
