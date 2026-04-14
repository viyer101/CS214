Varun Iyer (vmi14) Ahmed Elshenawy (ae508)
mysh.c --> The testing strategy focused on verifying that mysh correctly implements all required shell behaviors from the assignment. Testing covered both interactive and batch modes, along with core features such as parsing, execution, built-ins, redirection, pipelines, wildcard expansion, and error handling.
Three approaches were used:
    1) Interactive testing → prompt behavior, exit handling, status messages
    2) Batch/script testing → deterministic execution and ordering
    3) Filesystem-based testing → wildcard expansion, redirection, and directory operations
Special emphasis was also placed on correct behavior after failures (shell must not crash), built-ins affecting shell state (cd, exit), edge cases (invalid syntax, empty input, unmatched wildcards) as well as combined operations (pipes + redirection + built-ins)
Key Scenarios that we tested were:
  1) Interactive vs batch mode behavior
  2) Built-ins (cd, pwd, which, exit)
  3) Command parsing and tokenization
  4) Redirection (<, >)
  5) Pipelines (|)
  6) Wildcard expansion (*)
  7) Syntax errors and recovery
  8) Exit semantics (valid vs invalid usage)
  9) Batch-mode /dev/null handling
Test Cases implemented:
1. Startup & Modes
make clean && make
./mysh
./mysh script.sh
printf "echo hello\n" | ./mysh

2. Basic Commands
echo hello
pwd

3. Built-ins
cd subdir
pwd
cd ..
cd invalid_dir

which echo
which cd
which nonexistent

exit
exit 1

4. Comments & Empty Input
echo hi # inline comment


5. Redirection
cat < in.txt
echo hello > out.txt
cat < in.txt > out2.txt

6. Pipelines
echo hello | wc -c
echo hello | cat | wc -c
pwd | wc -c


7. Wildcards
echo *.txt
echo foo*bar
echo subdir/*.txt
echo xyz*

8. Error Handling
< <
echo hi |
| cat
echo hi >

9. Failure Handling

false
echo still_running

11. Exit Behavior
exit 1
echo still_here

echo hi | exit
echo hi | exit 1

11. Batch Mode /dev/null
printf "cat\n" | ./mysh
printf "cat < in.txt\n" | ./mysh
printf "echo hi | cat\n" | ./mysh


12. Combined Tests
echo *.txt | wc -w

pwd
which echo
badcmd
echo done

Correct chaining and continuation after failure
