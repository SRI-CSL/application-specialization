#!/usr/bin/env bash
# Make sure we exit if there is a failure
set -e


function usage() {
    echo "Usage: $0 [--disable-inlining] [--ipdse] [--use-crabopt] [--use-pointer-analysis] [--inter-spec VAL] [--intra-spec VAL] [--link dynamic|static] [--help]"
    echo "       VAL=none|aggressive|nonrec-aggressive"
}

#default values
INTER_SPEC="none"
INTRA_SPEC="none"
OPT_OPTIONS=""

POSITIONAL=()
while [[ $# -gt 0 ]]
do
key="$1"
case $key in
    -inter-spec|--inter-spec)
	INTER_SPEC="$2"
	shift # past argument
	shift # past value
	;;
    -intra-spec|--intra-spec)
	INTRA_SPEC="$2"
	shift # past argument
	shift # past value
	;;
    -disable-inlining|--disable-inlining)
	OPT_OPTIONS="${OPT_OPTIONS} --disable-inlining"
	shift # past argument
	;;
    -ipdse|--ipdse)
	OPT_OPTIONS="${OPT_OPTIONS} --ipdse"
	shift # past argument
	;;
    -use-crabopt|--use-crabopt)
	OPT_OPTIONS="${OPT_OPTIONS} --use-crabopt"
	shift # past argument
	;;
    -use-pointer-analysis|--use-pointer-analysis)
	OPT_OPTIONS="${OPT_OPTIONS} --use-pointer-analysis"	
	shift # past argument
	;;        
    -help|--help)
	usage
	exit 0
	;;
    *)    # unknown option
	POSITIONAL+=("$1") # save it in an array for later
	shift # past argument
	;;
esac
done
set -- "${POSITIONAL[@]}" # restore positional parameters


#check that the required dependencies are built
declare -a bitcode=(
	"netstat_modules/netstat.o.bc"
	"netstat_modules/statistics.o.bc"
	"netstat_modules/af.o.bc"
	"netstat_modules/getroute.o.bc"
	"netstat_modules/inet.o.bc"
	"netstat_modules/inet_gr.o.bc"
	"netstat_modules/interface.o.bc"
	"netstat_modules/nstrcmp.o.bc"
	"netstat_modules/proc.o.bc"
	"netstat_modules/sockets.o.bc"
	"netstat_modules/unix.o.bc"
	"netstat_modules/util.o.bc"
	"netstat_modules/hw.o.bc"
	"netstat_modules/loopback.o.bc"
	"netstat_modules/ether.o.bc"
)

for bc in "${bitcode[@]}"
do
    if [ -a  "$bc" ]
    then
        echo "Found $bc"
    else
        echo "Error: $bc not found. Try \"make\"."
        exit 1
    fi
done

SLASH_OPTS="--inter-spec-policy=${INTER_SPEC} --intra-spec-policy=${INTRA_SPEC}  --stats $OPT_OPTIONS"

# OCCAM with program and libraries dynamically linked
function dynamic_link() {

    export OCCAM_LOGLEVEL=INFO
    export OCCAM_LOGFILE=${PWD}/slash_specialized/occam.log

    rm -rf slash_specialized netstat_slashed

    echo "============================================================"
    echo "Running httpd with dynamic libraries "
    echo "slash options ${SLASH_OPTS}"
    echo "============================================================"
    slash ${SLASH_OPTS} --work-dir=slash_specialized \
	  --amalgamate=slash_specialized/amalgamation.bc \
	  netstat.json
    status=$?
    if [ $status -ne 0 ]
    then
	echo "Something failed while running slash"
	exit 1
    fi     
    cp ./slash_specialized/netstat_slashed .
}

dynamic_link
