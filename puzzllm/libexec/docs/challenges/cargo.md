# The Cargo Bay

Write a standalone Python 3 solver in `~/cargo.py`. Read one JSON object per line from standard input until EOF. Each object gives a cart capacity of six and a map from unique crate labels to positive integer weights. Print one JSON array per line. Each element is a cart containing an array of crate labels.

For input `{"capacity":6,"crates":{"A":2,"B":3,"C":4,"D":3}}` one valid output is `[["C","A"],["B","D"]]`. Cart and crate order are free.

Use every crate exactly once, stay within capacity, and minimize the number of carts. The grader makes 30 shuffled manifests by filling two, three or four carts exactly before removing the grouping. For a total weight of 18, three carts are both achievable and necessary. The same lower bound certifies the optimum in every manifest.
