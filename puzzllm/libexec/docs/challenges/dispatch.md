# The Dispatch Board

Write a standalone Python 3 solver in `~/dispatch.py`. Read one JSON object per line until EOF. In `{"jobs":["A","B","C"],"edges":[["A","C"]]}`, A must precede C and B has no listed constraint. Print one JSON object per line.

If the jobs can be scheduled, return every job exactly once in any valid order, for example `{"order":["B","A","C"]}`. Otherwise give a directed cycle, for example `{"cycle":["A","B","A"]}` when the board contains both edges. Repeat the first job at the end. All earlier jobs on the cycle must be distinct. A self-dependency has the witness `{"cycle":["A","A"]}`.

The grader checks each submitted edge or prerequisite. It tries every directed graph on three labeled jobs, including self-loops, and two six-job boards.
