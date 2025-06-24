# CDCL with F2 linear algebra

## Instructions for building
Compile Main.cpp, Solver.cpp, and REFMatrix.cpp with C++23.

The choice of variable order is defined with a compiler flag in Solver.h. Current options: standard basis in sequential order, random permutation of standard basis, random basis. Currently set to standard basis.

Current Main.cpp file is set up for diagnostic purposes, and will solve a file for at least 10 seconds, then output the average time used for solving. 

## Instructions for running

Provided build runs on macOS for ARM. 

Run `./Main -f path/to/test.txt`, for example `./Main -f tests/bphp5-4.txt`.