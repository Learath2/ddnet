#!/bin/bash

arg_verbose=0
arg_valgrind_memcheck=0

for arg in "$@"
do
	if [ "$arg" == "-h" ] || [ "$arg" == "--help" ]
	then
		echo "usage: $(basename "$0") [OPTION..] [build dir]"
		echo "description:"
		echo "  Runs a simple integration test of the client and server"
		echo "  binaries from the given build dir"
		echo "options:"
		echo "  --help|-h           show this help"
		echo "  --verbose|-v        verbose output"
		echo "  --valgrind-memcheck use valgrind's memcheck to run server and client"
		exit 0
	elif [ "$arg" == "-v" ] || [ "$arg" == "--verbose" ]
	then
		arg_verbose=1
	elif [ "$arg" == "--valgrind-memcheck" ]
	then
		arg_valgrind_memcheck=1
	else
		echo "Error: unknown arg '$arg'"
		exit 1
	fi
done

if [ ! -f DDNet ]
then
	echo "Error: client binary not found DDNet' not found"
	exit 1
fi
if [ ! -f DDNet-Server ]
then
	echo "Error: server binary not found DDNet-Server' not found"
	exit 1
fi

got_killed=0

function kill_all() {
	# needed to fix hang fifo with additional ctrl+c
	if [ "$got_killed" == "1" ]
	then
		exit
	fi
	got_killed=1

	if [ "$arg_verbose" == "1" ]
	then
		echo "[*] shutting down test clients and server ..."
	fi

	sleep 1
	echo "shutdown" > server.fifo
	echo "quit" > client1.fifo
	echo "quit" > client2.fifo
}

function cleanup() {
	kill_all
}

trap cleanup EXIT

function fail()
{
	sleep 1
	tail -n2 "$1".log > fail_"$1".txt
	echo "$1 exited with code $2" >> fail_"$1".txt
	echo "[-] $1 exited with code $2"
}

# TODO: check for open ports instead
port=17822

if [[ $OSTYPE == 'darwin'* ]]; then
	DETECT_LEAKS=0
else
	DETECT_LEAKS=1
fi

export UBSAN_OPTIONS=suppressions=../ubsan.supp:log_path=./SAN:print_stacktrace=1:halt_on_errors=0
export ASAN_OPTIONS=log_path=./SAN:print_stacktrace=1:check_initialization_order=1:detect_leaks=$DETECT_LEAKS:halt_on_errors=0
export LSAN_OPTIONS=suppressions=../lsan.supp:print_suppressions=0

function print_results() {
	if [ "$arg_valgrind_memcheck" == "1" ]; then
		if grep "ERROR SUMMARY" server.log client1.log client2.log | grep -q -v "ERROR SUMMARY: 0"; then
			grep "^==" server.log client1.log client2.log
			return 1
		fi
	else
		if test -n "$(find . -maxdepth 1 -name 'SAN.*' -print -quit)"
		then
			cat SAN.*
			return 1
		fi
	fi
	return 0
}

rm -rf integration_test
mkdir -p integration_test/data/maps
cp data/maps/coverage.map integration_test/data/maps
cp data/maps/Tutorial.map integration_test/data/maps
cd integration_test || exit 1

{
	echo $'add_path $CURRENTDIR'
	echo $'add_path $USERDIR'
	echo $'add_path $DATADIR'
	echo $'add_path ../data'
} > storage.cfg

if [ "$arg_valgrind_memcheck" == "1" ]; then
	tool="valgrind --tool=memcheck --gen-suppressions=all --suppressions=../memcheck.supp --track-origins=yes"
	client_args="cl_menu_map \"\";"
else
	tool=""
	client_args=""
fi

function wait_for_fifo() {
	local fifo="$1"
	local tries="$2"
	local fails=0
	# give the client time to launch and create the fifo file
	# but assume after X secs that the client crashed before
	# being able to create the file
	while [[ ! -p "$fifo" ]]
	do
		fails="$((fails+1))"
		if [ "$arg_verbose" == "1" ]
		then
			echo "[!] client fifos not found (attempts $fails/$tries)"
		fi
		if [ "$fails" -gt "$tries" ]
		then
			print_results
			echo "[-] Error: client possibly crashed on launch"
			exit 1
		fi
		sleep 1
	done
}

$tool ../DDNet-Server \
	"sv_input_fifo server.fifo;
	sv_rcon_password rcon;
	sv_map coverage;
	sv_sqlite_file ddnet-server.sqlite;
	logfile server.log;
	sv_port $port" > stdout_server.txt 2> stderr_server.txt || fail server "$?" &

$tool ../DDNet \
	"cl_input_fifo client1.fifo;
	player_name client1;
	cl_download_skins 0;
	gfx_fullscreen 0;
	logfile client1.log;
	$client_args
	connect localhost:$port" > stdout_client1.txt 2> stderr_client1.txt || fail client1 "$?" &

if [ "$arg_valgrind_memcheck" == "1" ]; then
	wait_for_fifo client1.fifo 120
	sleep 1
else
	wait_for_fifo client1.fifo 50
	sleep 1
fi

$tool ../DDNet \
	"cl_input_fifo client2.fifo;
	player_name client2;
	cl_download_skins 0;
	gfx_fullscreen 0;
	logfile client2.log;
	$client_args
	connect localhost:$port" > stdout_client2.txt 2> stderr_client2.txt || fail client2 "$?" &

if [ "$arg_valgrind_memcheck" == "1" ]; then
	wait_for_fifo client2.fifo 120
	sleep 20
else
	wait_for_fifo client2.fifo 50
	sleep 2
fi

echo "[*] test chat and chat commands"
echo "say hello world" > client1.fifo
echo "rcon_auth rcon" > client1.fifo
sleep 1
tr -d '\n' > client1.fifo << EOF
say "/mc
;top5
;rank
;team 512
;emote happy -999
;pause
;points
;mapinfo
;list
;whisper client2 hi
;kill
;settings cheats
;timeout 123
;timer broadcast
;cmdlist
;saytime"
EOF
sleep 1
echo "[*] test rcon commands"
tr -d '\n' > client1.fifo << EOF
rcon say hello from admin;
rcon broadcast test;
rcon status;
rcon echo test;
muteid 1 900 spam;
unban_all;
EOF
sleep 1
echo "[*] test map change"
echo "rcon sv_map Tutorial" > client1.fifo
sleep 1


# TODO: remove the first grep after https://github.com/ddnet/ddnet/pull/5036 is merged
if ! grep -qE '^\[[0-9]{4}-[0-9]{2}-[0-9]{2} ([0-9]{2}:){2}[0-9]{2}\]\[chat\]: 0:-2:client1: hello world$' server.log && \
	! grep -qE '^[0-9]{4}-[0-9]{2}-[0-9]{2} ([0-9]{2}:){2}[0-9]{2} D chat: 0:-2:client1: hello world$' server.log
then
	touch fail_chat.txt
	echo "[-] Error: chat message not found in server log"
fi
if ! grep -q 'cmdlist' client1.log || \
	! grep -q 'pause' client1.log || \
	! grep -q 'rank' client1.log || \
	! grep -q 'points' client1.log
then
	touch fail_chatcommand.txt
	echo "[-] Error: did not find output of /cmdlist command"
fi

if ! grep -q "hello from admin" server.log
then
	touch fail_rcon.txt
	echo "[-] Error: admin message not found in server log"
fi

kill_all
wait

sleep 1

ranks="$(sqlite3 ddnet-server.sqlite < <(echo "select * from record_race;"))"
num_ranks="$(echo "$ranks" | wc -l | xargs)"
if [ "$ranks" == "" ]
then
	touch fail_ranks.txt
	echo "[-] Error: no ranks found in database"
elif [ "$num_ranks" != "1" ]
then
	touch fail_ranks.txt
	echo "[-] Error: expected 1 rank got $num_ranks"
elif ! echo "$ranks" | grep -q client1
then
	touch fail_ranks.txt
	echo "[-] Error: expected a rank from client1 instead got:"
	echo "  $ranks"
fi

for logfile in client1.log client2.log server.log
do
	if [ "$arg_valgrind_memcheck" == "1" ]
	then
		break
	fi
	if [ ! -f "$logfile" ]
	then
		echo "[-] Error: logfile '$logfile' not found."
		touch fail_logs.txt
		continue
	fi
	logdiff="$(diff "$logfile" "stdout_$(basename "$logfile" .log).txt")"
	if [ "$logdiff" != "" ]
	then
		echo "[-] Error: logfile '$logfile' differs from stdout"
		echo "[-] Error: logfile '$logfile' differs from stdout" >> fail_logs.txt
		echo "$logdiff" >> fail_logs.txt
	fi
done

for stderr in ./stderr_*.txt
do
	if [ ! -f "$stderr" ]
	then
		continue
	fi
	if [ "$(cat "$stderr")" == "" ]
	then
		continue
	fi
	if [ "$arg_verbose" != "1" ]
	then
		continue
	fi
	echo "[!] Warning: $stderr"
	cat "$stderr"
done

if test -n "$(find . -maxdepth 1 -name 'fail_*' -print -quit)"
then
	if [ "$arg_verbose" == "1" ]
	then
		for fail in fail_*
		do
			cat "$fail"
		done
	fi
	print_results
	echo "[-] Test failed. See errors above."
	exit 1
else
	echo "[*] all tests passed"
fi

print_results || exit 1