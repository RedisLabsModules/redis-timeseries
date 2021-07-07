#!/bin/bash

[[ $VERBOSE == 1 ]] && set -x
[[ $IGNERR == 1 ]] || set -e

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" >/dev/null 2>&1 && pwd)"
ROOT=$(cd $HERE/../.. && pwd)
READIES=$ROOT/deps/readies 
. $READIES/shibumi/defs

VALGRIND_REDIS_VER=6.2

#----------------------------------------------------------------------------------------------

help() {
	cat <<-END
		Run flow tests.

		[ARGVARS...] tests.sh [--help|help] [<module-so-path>]

		Argument variables:
		GEN=0|1             General tests
		AOF=0|1             Tests with --test-aof
		SLAVES=0|1          Tests with --test-slaves
		CLUSTER=0|1         Tests with --env oss-cluster

        REDIS_SERVER=path   Location of redis-server
		GEARS=0|1           Tests with RedisGears
        GEARS_PATH=path     Path to redisgears.so

		TEST=test           Run specific test (e.g. test.py:test_name)
		VALGRIND|VG=1       Run with Valgrind

		VERBOSE=1           Print commands
		IGNERR=1            Do not abort on error

	END
}

#----------------------------------------------------------------------------------------------

setup_redis_server() {
	# if [[ $VALGRIND == 1 ]]; then
	# 	REDIS_SERVER=${REDIS_SERVER:-redis-server-vg}
	# 	if ! is_command $REDIS_SERVER; then
	# 		echo Building Redis for Valgrind ...
	# 		$READIES/bin/getredis -v $VALGRIND_REDIS_VER --valgrind --suffix vg
	# 	fi
	# else
		REDIS_SERVER=${REDIS_SERVER:-redis-server}
	# fi

	if ! is_command $REDIS_SERVER; then
		echo "Cannot find $REDIS_SERVER. Aborting."
		exit 1
	fi
}

#----------------------------------------------------------------------------------------------

valgrind_config() {
	export VG_OPTIONS="
		-q \
		--leak-check=full \
		--show-reachable=no \
		--track-origins=yes \
		--show-possibly-lost=no"

	# To generate supressions and/or log to file
	# --gen-suppressions=all --log-file=valgrind.log

	VALGRIND_SUPRESSIONS=$ROOT/tests/redis_valgrind.sup

	VALGRIND_ARGS+="\
		--no-output-catch \
		--use-valgrind \
		--vg-verbose \
		--vg-suppressions $VALGRIND_SUPRESSIONS"

}

#----------------------------------------------------------------------------------------------

run_tests() {
	local title="$1"
	if [[ -n $title ]]; then
		$READIES/bin/sep -0
		printf "Tests with $title:\n\n"
	fi

	config=$(mktemp "${TMPDIR:-/tmp}/rltest.XXXXXXX")
	rm -f $config
	cat << EOF > $config

--clear-logs
--oss-redis-path=$REDIS_SERVER
--module $MODULE
--module-args '$MODARGS'
$RLTEST_ARGS
$GEARS_ARGS
$VALGRIND_ARGS

EOF

	cd $ROOT/tests/flow
	if [[ $VERBOSE == 1 ]]; then
		echo "RLTest configuration:"
		cat $config
	fi
	$OP python3 -m RLTest @$config
	[[ $KEEP != 1 ]] && rm -f $config
}

#----------------------------------------------------------------------------------------------

[[ $1 == --help || $1 == help ]] && {
	help
	exit 0
}

GEN=${GEN:-1}
SLAVES=${SLAVES:-1}
AOF=${AOF:-1}
CLUSTER=${CLUSTER:-1}
GEARS=${GEARS:-0}

GDB=${GDB:-0}

OP=""
[[ $NOP == 1 ]] && OP=echo

MODULE=${MODULE:-$1}
[[ -z $MODULE || ! -f $MODULE ]] && {
	echo "Module not found at ${MODULE}. Aborting."
	exit 1
}

[[ $VG == 1 ]] && VALGRIND=1
if [[ $VALGRIND == 1 ]]; then
	valgrind_config
fi

if [[ -n $TEST ]]; then
	RLTEST_ARGS+=" --test $TEST -s"
	export BB=${BB:-1}
fi

[[ $VERBOSE == 1 ]] && RLTEST_ARGS+=" -v"
[[ $GDB == 1 ]] && RLTEST_ARGS+=" -i --verbose"

#----------------------------------------------------------------------------------------------

GEARS_BRANCH=${GEARS_BRANCH:-master}
if [[ -n $GEARS && $GEARS != 0 ]]; then
	platform=`$READIES/bin/platform -t`
	if [[ -n $GEARS_PATH ]]; then
		GEARS_ARGS="--module $GEARS_PATH"
		GEARS_MODULE="$GEARS_PATH"
	else
		GEARS_MODULE="$ROOT/bin/$platform/RedisGears/redisgears.so"
		if [[ ! -f $GEARS_MODULE || $GEARS == get ]]; then
			runn BRANCH=$GEARS_BRANCH $OP $ROOT/sbin/getgears
		fi
		GEARS_ARGS="--module $GEARS_MODULE"
	fi
	GEARS_ARGS+=" --module-args '$GEARS_MODARGS'"
fi

#----------------------------------------------------------------------------------------------

cd $ROOT/tests/flow

setup_redis_server

[[ $GEN == 1 ]] && run_tests
[[ $CLUSTER == 1 ]] && RLTEST_ARGS+=" --env oss-cluster --shards-count 2" run_tests "oss-cluster"
[[ $SLAVES == 1 ]] && RLTEST_ARGS+=" --use-slaves" run_tests "with slaves"
[[ $AOF == 1 ]] && RLTEST_ARGS+=" --use-aof" run_tests "with AOF"

exit 0
