# The Freight Lift

Write a standalone Python 3 program in `~/lift.py`. Read standard input one line at a time until EOF. Each line is a separate journey of zero to eight commands. Start every journey at floor zero. U goes up one floor and D goes down one.

Print the 1-based step on which the lift first reaches floor −1, or `OK` if it stays at or above ground. The empty input line is a journey and needs its own `OK`. Print exactly one answer per line. The door checks all 511 journeys.

Try `printf '\nUUDD\nUDDU\nD\n' | python3 ~/lift.py`. The four answers are `OK`, `OK`, `3`, and `1`.
