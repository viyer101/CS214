Varun Iyer (vmi14) Ahmed Elshenawy (ae508)
compare.c - This code analyzes a set of input files and computes the Jensen-Shannon Distance between every pair based on their word frequency distributions. This program supports directories and files, it traverses throguh the files recursively and includes only files that are a matching pair (I.E. both files must be txt, you can't have different file structures).
Makefile - This file will help compile and generate a file called compare. You have to type: make.
How to run compare - as stated above, you have to have two similar file structures in order to compare them, you would then type: ./compare <file-or-directory> [more files/directories...], heres an example:
./compare [file directory 1], [file directory 2].
testing - This folder has a bunch of files that test a lot of files and check their distances, with some hidden files that won't be read.
EXAMPLE: ./compare testing all the readable files in the directory, and it traverses directories and calculates their distance and checks punctuations.
./compare testing/tests lists all readable files in the tests subdirectory only.
./compare testing/testdir will skip both .hdir and .hidden.txt, because both of these are unreadable, and it won't check .hdir directory.
./compare testing/tests/hyphen will lists all readable files and caclulate the distance, reading the hyphens.
./compare testing/pdf_tests/token testing/pdf_tests/known/a.txt will display the length on how long it will take to reach the files, the max is 1 second.
./compare testing/test/suffix will not read b.c, since the files can only read and write .txt files.