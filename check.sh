#!/bin/bash


set -e


NPROCESS=4
FIRSTPORT=9000
CTYPES="classic unbatch"
SETUPTIME=5
TESTTIME=30


# Program compilation - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

if [ ! -e Makefile ] ; then
    echo "Prepare Salticidae building"
    cmake .
fi

if [ ! -e libsalticidae.a ] ; then
    echo "Build Salticidae"
    make -j8
fi

if [ ! -e unbatch -o unbatch -ot main.cpp ] ; then
    echo "Build test program"
    g++ -Wall -Wextra -O3 -g3 -std=c++17 main.cpp -o unbatch -Iinclude \
	-L. -luv -lcrypto -lssl -lsalticidae -pthread
fi


# Test setup  - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

echo "Prepare test: ${NPROCESS} process"

ips=

for i in $(seq 0 $(( NPROCESS - 1 ))) ; do
    ip="localhost:$(( FIRSTPORT + i ))"
    echo "  add local ip: ${ip}"
    ips="${ips} ${ip}"
done


# Test execution  - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

pids=

atexit() {
    if [ "x${pids}" != 'x' ] ; then
	echo "Kill all process: ${pids}"
	kill ${pids}
	wait ${pids}
    fi
}

trap atexit 'EXIT'

for ctype in ${CTYPES} ; do

    echo "Execute test for network type: ${ctype}"

    echo "  start ${NPROCESS} process"

    for i in $(seq 0 $(( NPROCESS - 1 ))) ; do
	log="check-${ctype}.${i}.log"

	./unbatch "${ctype}" ${i} ${ips} > "${log}" 2>&1 &
	pid=$!

	if [ ${i} -eq 0 ] ; then
	    echo "    start leader process: ${pid} > ${log}"
	    leader=${pid}
	else
	    echo "    start follower process: ${pid} > ${log}"
	fi

	pids="${pids} ${pid}"
    done

    echo "  wait ${SETUPTIME} seconds for network stabilization..."

    sleep ${SETUPTIME}

    echo "  send SIGINT signal to leader ${leader} to start request generation"

    kill -INT ${leader}

    echo "  wait ${TESTTIME} seconds for running test..."

    sleep ${TESTTIME}

    echo "  send SIGINT signal to all processes to stop"

    kill -INT ${pids}

    wait ${pids}

    echo "  test complete for network type ${ctype}"

    pids=
done


# Result display  - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

echo "Throughput results:"

for ctype in ${CTYPES} ; do
    leaderlog="check-${ctype}.0.log"
 
    thr=$(sed -rn 's/^.*propose: (.+)$/\1/p' "${leaderlog}" | tail -1)
    tps=$(( thr / TESTTIME ))

    echo "  ${ctype}: ${tps} tx/s (${thr} tx in ${TESTTIME} seconds)"

done
