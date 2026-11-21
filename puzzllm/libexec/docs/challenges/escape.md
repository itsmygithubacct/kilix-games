# The Emergency Exit

Write a standalone Python 3 solver in `~/escape.py`. Each input line gives a 4-column by 3-row board as twelve characters, read left to right starting with the top row. A dot is an open cell and a hash is a wall. Start in the top-left cell and finish in the bottom-right. Both corners are always open.

Print a shortest legal route as a string of U, D, L and R, or `BLOCKED` if there is none. Each move goes one cell vertically or horizontally. Read until EOF and print exactly one answer per input line.

For `............`, both `RRRDD` and `DDRRR` are shortest legal answers. For `.#..#.......`, the start is cut off. The checker compares your route with a breadth-first search on each of the 1,024 possible layouts.
