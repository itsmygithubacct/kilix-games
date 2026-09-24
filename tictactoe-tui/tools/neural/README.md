# tictactoe-tui neural player: training

`train.py` builds the whole training set from the game itself: every
position reachable from an empty board with a move to make (4,520), each
legal move labelled with its exact minimax value for the mover, scaled so
that faster wins and slower losses score higher. Nothing else goes in: no
third-party data or weights.

The network (18 → 128 → 128 → 9, ReLU) is fit with full-batch Adam to
masked MSE on those values plus 0.1 × cross-entropy toward the best moves.
Training stops at the first check (every 250 epochs, from epoch 2000) where
the gate holds over all 4,520 positions:

1. the best legal move by the network is optimal (never gives up a win or a
   draw);
2. when a win exists, it is a fastest win.

Only then are the blob (`assets/policy/tictactoe-neural.kxpol`, packed by
kilix-game-kit's `tools/kilix_policy.py`), the embedded header and
`docs/neural-policy-provenance.json` written. The game's `--neural-test`
then re-proves the gate in C against an independent solver and plays the
network against every opponent line from both sides.

```sh
make train          # retrain and reinstall (numpy; a few minutes on one core)
make check-policy   # blob, header and manifest agree
make test
```
