#!/bin/bash

make -j2 mdbx_test && rm -f /dev/shm/mdbx-test.* *.log
valgrind --trace-children=yes --log-file=valgrind-%p.log --leak-check=full --track-origins=yes --error-exitcode=42 --suppressions=test/valgrind_suppress.txt \
--vgdb=yes --vgdb-error=2 \
./mdbx_test basic > test.log
