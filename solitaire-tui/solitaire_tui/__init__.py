"""solitaire-tui: Klondike for the terminal, structured for learned players.

Layers, from the bottom up:
  engine   pure rules (State, Move, deal, legal_moves, apply)
  actions  fixed 681-way action space and legality masks
  observe  fixed 690-float player-visible observation
  agents   non-human players (random, greedy, PolicyAgent hook)
  features per-move features for the neural player (player-visible only)
  planner  determinized rollout planner (the neural player's teacher)
  policy   the shipped neural player, a pure-Python KXPOLICY reader
  sim      headless games, tournaments and trajectories
  tui      the curses game
"""
__version__ = "0.2.0"
