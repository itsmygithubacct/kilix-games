PYTHON ?= python3

.PHONY: test run simulate clean

test:
	PYTHONDONTWRITEBYTECODE=1 $(PYTHON) -m unittest discover -s tests -t .

run:
	./bin/solitaire-tui

simulate:
	./bin/solitaire-tui --simulate greedy --games 200

clean:
	find . -name __pycache__ -type d -prune -exec rm -rf {} +
