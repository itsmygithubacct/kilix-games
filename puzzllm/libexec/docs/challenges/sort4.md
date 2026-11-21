# The Four-Lane Sorter

Write a sorting network in `~/sort4.net`. Each nonblank line holds two lane numbers separated by whitespace, lower lane number first. Lanes are numbered 1 through 4. For example, `1 3` puts the lighter of those two items in lane 1 and the heavier in lane 3.

Choose at most five pairs. Every input passes through exactly those pairs in file order. The output must increase from lane 1 to lane 4, allowing ties. Submit just the pairs.

The checker shows the first binary input that fails. Checking all 16 patterns proves the network for arbitrary weights. If a heavier item ever finished before a lighter one, thresholding between their weights would produce a binary input with the same inversion. A threshold commutes with each compare–exchange.
