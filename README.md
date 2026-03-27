Varun Iyer (vmi14) Ahmed Elshenawy (ae508)
compare.c - This code analyzes a set of input files and computes the Jensen-Shannon Distance between every pair based on their word frequency distributions. This program supports directories and files, it traverses throguh the files recursively and includes only files that are a matching pair (I.E. both files must be txt, you can't have different file structures).
Makefile - This file will help compile and generate a file called compare. You have to type: make.
How to run compare - as stated above, you have to have two similar file structures in order to compare them, you would then type: ./compare <file-or-directory> [more files/directories...], heres an example:
./compare foo.txt data/